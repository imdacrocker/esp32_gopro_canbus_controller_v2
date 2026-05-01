/*
 * can_manager.c — CAN bus manager (§14).
 *
 * Hardware: 1 Mbps, TX=GPIO7, RX=GPIO6, standard 11-bit IDs.
 * Receives:  0x600 (RaceCapture logging command), 0x602 (GPS UTC timestamp).
 * Transmits: 0x601 (camera status) at 5 Hz via esp_timer.
 *
 * RX model: on_rx_done ISR callback reads the frame and enqueues it to a
 * FreeRTOS queue; the RX task (priority 5, core 1) dequeues and processes.
 */

#include "can_manager.h"
#include "camera_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "can_manager";

/* ---- Hardware constants (§14.1) ------------------------------------------ */

#define CAN_TX_GPIO          7
#define CAN_RX_GPIO          6
#define CAN_TX_QUEUE_DEPTH   8

/* ---- Protocol constants (§14.2) ------------------------------------------ */

#define CAN_ID_LOGGING_CMD   0x600u
#define CAN_ID_CAM_STATUS    0x601u
#define CAN_ID_GPS_UTC       0x602u

#define STATUS_TX_PERIOD_US  200000   /* 5 Hz */
#define WATCHDOG_TIMEOUT_US  5000000  /* 5 s  */

/* First valid 0x602 timestamp: 2020-01-01 00:00:00 UTC in ms */
#define UTC_MIN_VALID_MS     1577836800000ULL

/* ---- NVS ------------------------------------------------------------------ */

#define NVS_NAMESPACE        "can_mgr"
#define NVS_KEY_TZ_OFFSET    "tz_off"

/* ---- RX queue item -------------------------------------------------------- */

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[TWAI_FRAME_MAX_LEN];
} can_rx_item_t;

/* Depth matches the legacy RX queue depth from §14.1. */
#define RX_QUEUE_DEPTH       32

/* ---- State ---------------------------------------------------------------- */

static can_manager_callbacks_t s_cbs;
static twai_node_handle_t      s_node;
static QueueHandle_t           s_rx_queue;

/* Logging state: written only from RX task, read from any task.
 * Aligned 4-byte enum — single-word writes are atomic on Xtensa LX7. */
static volatile can_logging_state_t s_logging_state = LOGGING_STATE_UNKNOWN;

/* UTC timestamp state — multi-field, protected by mutex. */
static SemaphoreHandle_t s_utc_mutex;
static struct {
    bool     valid;
    bool     acquired_fired;
    uint64_t last_utc_ms;
    int64_t  last_esp_us;   /* esp_timer_get_time() snapshot at last 0x602 */
} s_utc;

static int8_t s_tz_offset = 0;

static esp_timer_handle_t s_tx_timer;
static esp_timer_handle_t s_watchdog_timer;
static TaskHandle_t       s_rx_task_handle;

/* ---- Watchdog ------------------------------------------------------------- */

static void watchdog_cb(void *arg)
{
    /* 5 s elapsed without a 0x600 frame — suppress mismatch correction. */
    s_logging_state = LOGGING_STATE_UNKNOWN;
    camera_manager_set_desired_recording_all(DESIRED_RECORDING_UNKNOWN);
    /* Callback is NOT fired with UNKNOWN (§14.2). */
}

/* ---- 0x600 handler -------------------------------------------------------- */

static void handle_logging_cmd(const can_rx_item_t *item)
{
    can_logging_state_t state = item->data[0]
        ? LOGGING_STATE_LOGGING
        : LOGGING_STATE_NOT_LOGGING;

    s_logging_state = state;

    /* Reset 5 s watchdog. */
    esp_timer_stop(s_watchdog_timer);
    esp_timer_start_once(s_watchdog_timer, WATCHDOG_TIMEOUT_US);

    /* Notify camera_manager (idempotent — see §13.2). */
    camera_manager_set_desired_recording_all(
        state == LOGGING_STATE_LOGGING
            ? DESIRED_RECORDING_START
            : DESIRED_RECORDING_STOP);

    if (s_cbs.on_logging_state) {
        s_cbs.on_logging_state(state, s_cbs.on_logging_state_arg);
    }
}

/* ---- 0x602 handler -------------------------------------------------------- */

static void handle_gps_utc(const can_rx_item_t *item)
{
    if (item->dlc < 8) {
        return;
    }

    uint64_t utc_ms;
    memcpy(&utc_ms, item->data, sizeof(utc_ms));   /* little-endian on-wire */

    if (utc_ms < UTC_MIN_VALID_MS) {
        return;   /* GPS not yet locked or stale */
    }

    xSemaphoreTake(s_utc_mutex, portMAX_DELAY);
    bool first           = !s_utc.acquired_fired;
    s_utc.valid          = true;
    s_utc.acquired_fired = true;
    s_utc.last_utc_ms    = utc_ms;
    s_utc.last_esp_us    = esp_timer_get_time();
    xSemaphoreGive(s_utc_mutex);

    if (first && s_cbs.on_utc_acquired) {
        s_cbs.on_utc_acquired(utc_ms, s_cbs.on_utc_acquired_arg);
    }
}

/* ---- ISR: on_rx_done — reads frame and enqueues to RX task --------------- */

static bool IRAM_ATTR on_rx_done_isr(twai_node_handle_t handle,
                                      const twai_rx_done_event_data_t *edata,
                                      void *user_ctx)
{
    can_rx_item_t item;
    twai_frame_t frame = {
        .buffer     = item.data,
        .buffer_len = sizeof(item.data),
    };

    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }

    item.id  = frame.header.id;
    item.dlc = (uint8_t)frame.header.dlc;

    BaseType_t higher_prio_woken = pdFALSE;
    xQueueSendFromISR(s_rx_queue, &item, &higher_prio_woken);
    return higher_prio_woken == pdTRUE;
}

/* ---- RX task (§14.4 — priority 5, core 1) -------------------------------- */

static void rx_task(void *arg)
{
    can_rx_item_t item;

    for (;;) {
        if (!xQueueReceive(s_rx_queue, &item, portMAX_DELAY)) {
            continue;
        }

        if (s_cbs.on_rx_frame) {
            s_cbs.on_rx_frame(item.id, item.data, item.dlc,
                               s_cbs.on_rx_frame_arg);
        }

        switch (item.id) {
        case CAN_ID_LOGGING_CMD:
            handle_logging_cmd(&item);
            break;
        case CAN_ID_GPS_UTC:
            handle_gps_utc(&item);
            break;
        default:
            break;
        }
    }
}

/* ---- 0x601 TX timer (5 Hz) ------------------------------------------------ */

static void tx_timer_cb(void *arg)
{
    uint8_t data[CAMERA_MAX_SLOTS];
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        data[i] = (uint8_t)camera_manager_get_slot_can_state(i);
    }

    twai_frame_t frame = {
        .header = {
            .id  = CAN_ID_CAM_STATUS,
            .dlc = CAMERA_MAX_SLOTS,
            .ide = 0,
            .rtr = 0,
            .fdf = 0,
        },
        .buffer     = data,
        .buffer_len = sizeof(data),
    };

    /* Non-blocking — drop the frame if the TX queue is full. */
    esp_err_t err = twai_node_transmit(s_node, &frame, 0);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "twai_node_transmit: %s", esp_err_to_name(err));
    }
}

/* ---- NVS ------------------------------------------------------------------ */

static void load_tz_offset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int8_t val;
    if (nvs_get_i8(h, NVS_KEY_TZ_OFFSET, &val) == ESP_OK) {
        s_tz_offset = val;
    }
    nvs_close(h);
}

static void save_tz_offset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_i8(h, NVS_KEY_TZ_OFFSET, s_tz_offset);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- Public API ----------------------------------------------------------- */

void can_manager_register_callbacks(const can_manager_callbacks_t *cbs)
{
    s_cbs = *cbs;
}

void can_manager_init(void)
{
    load_tz_offset();

    s_utc_mutex = xSemaphoreCreateMutex();
    configASSERT(s_utc_mutex);

    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(can_rx_item_t));
    configASSERT(s_rx_queue);

    /* TWAI node (§14.1). */
    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx               = CAN_TX_GPIO,
            .rx               = CAN_RX_GPIO,
            .quanta_clk_out   = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = {
            .bitrate = 1000000,   /* 1 Mbps */
        },
        .tx_queue_depth = CAN_TX_QUEUE_DEPTH,
        .fail_retry_cnt = 0,      /* single attempt — drop on ACK error (no receiver connected) */
    };
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_cfg, &s_node));

    twai_event_callbacks_t twai_cbs = {
        .on_rx_done = on_rx_done_isr,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(s_node, &twai_cbs, NULL));

    ESP_ERROR_CHECK(twai_node_enable(s_node));
    ESP_LOGI(TAG, "TWAI started at 1 Mbps (TX=%d RX=%d)", CAN_TX_GPIO, CAN_RX_GPIO);

    /* 5 s watchdog — fires if no 0x600 frame arrives. */
    esp_timer_create_args_t wd_args = {
        .callback = watchdog_cb,
        .name     = "can_watchdog",
    };
    ESP_ERROR_CHECK(esp_timer_create(&wd_args, &s_watchdog_timer));

    /* 0x601 TX timer at 5 Hz. */
    esp_timer_create_args_t tx_args = {
        .callback = tx_timer_cb,
        .name     = "can_tx",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tx_args, &s_tx_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tx_timer, STATUS_TX_PERIOD_US));

    /* RX task on core 1, priority 5 (§14.4). */
    xTaskCreatePinnedToCore(rx_task, "can_rx", 4096, NULL, 5,
                             &s_rx_task_handle, 1);
    configASSERT(s_rx_task_handle);
}

can_logging_state_t can_manager_get_logging_state(void)
{
    return s_logging_state;
}

bool can_manager_get_utc_ms(uint64_t *out_ms)
{
    xSemaphoreTake(s_utc_mutex, portMAX_DELAY);
    if (!s_utc.valid) {
        xSemaphoreGive(s_utc_mutex);
        return false;
    }
    int64_t elapsed_us = esp_timer_get_time() - s_utc.last_esp_us;
    *out_ms = s_utc.last_utc_ms + (uint64_t)(elapsed_us / 1000);
    xSemaphoreGive(s_utc_mutex);
    return true;
}

void can_manager_set_tz_offset(int8_t hours)
{
    if (hours < -12) hours = -12;
    if (hours >  14) hours =  14;
    s_tz_offset = hours;
    save_tz_offset();
}

int8_t can_manager_get_tz_offset(void)
{
    return s_tz_offset;
}

esp_err_t can_manager_set_manual_utc_ms(uint64_t utc_ms)
{
    if (utc_ms < UTC_MIN_VALID_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_utc_mutex, portMAX_DELAY);
    if (s_utc.valid) {
        xSemaphoreGive(s_utc_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_utc.valid          = true;
    s_utc.acquired_fired = true;
    s_utc.last_utc_ms    = utc_ms;
    s_utc.last_esp_us    = esp_timer_get_time();
    xSemaphoreGive(s_utc_mutex);

    /* Fire the one-time callback — same path as a real GPS acquisition. */
    if (s_cbs.on_utc_acquired) {
        s_cbs.on_utc_acquired(utc_ms, s_cbs.on_utc_acquired_arg);
    }

    ESP_LOGI(TAG, "manual UTC set: %llu ms", (unsigned long long)utc_ms);
    return ESP_OK;
}

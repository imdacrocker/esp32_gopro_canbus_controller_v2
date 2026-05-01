/*
 * driver.c — Component init, per-slot context table, and driver vtable.
 *
 * Threading model:
 *   - start_recording / stop_recording are called from camera_manager's
 *     mismatch poll timer (ESP_TIMER_TASK) — they MUST NOT block.
 *     They post a command to cmd_queue and return immediately.
 *   - The per-slot http_worker_task executes commands and periodic polls.
 *   - get_recording_status() returns ctx->cached_status under a mutex —
 *     safe from any context with no blocking I/O.
 *   - on_wifi_associated() and on_wifi_disconnected() are called on the
 *     wifi_manager event task — they must not block.
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "camera_manager.h"
#include "open_gopro_ble.h"
#include "gopro_model.h"
#include "open_gopro_http.h"
#include "open_gopro_http_internal.h"

static const char *TAG = "gopro_http";

/* ---- Per-slot context table ---------------------------------------------- */

static gopro_http_ctx_t s_ctx[CAMERA_MAX_SLOTS];

/* ---- Worker task ---------------------------------------------------------- */

static void http_worker_task(void *arg)
{
    gopro_http_ctx_t *ctx = (gopro_http_ctx_t *)arg;

    /* Probe blocks until probe succeeds or exhausts retries. */
    gopro_http_probe(ctx);

    gopro_http_cmd_t cmd;
    while (!ctx->stop_requested) {
        /*
         * Block for one poll interval.  If a command arrives earlier, execute
         * it immediately; otherwise fall through to the periodic status poll.
         */
        if (xQueueReceive(ctx->cmd_queue, &cmd,
                          pdMS_TO_TICKS(GOPRO_HTTP_POLL_INTERVAL_MS)) == pdTRUE) {
            if (ctx->stop_requested) break;

            const char *path = (cmd == HTTP_CMD_START_RECORDING)
                             ? GOPRO_HTTP_PATH_SHUTTER_START
                             : GOPRO_HTTP_PATH_SHUTTER_STOP;
            const char *label = (cmd == HTTP_CMD_START_RECORDING)
                              ? "start_recording" : "stop_recording";

            int code = gopro_http_get(ctx, path, NULL, 0);
            if (code == 200) {
                ESP_LOGI(TAG, "slot %d: %s OK", ctx->slot, label);
            } else {
                ESP_LOGW(TAG, "slot %d: %s → HTTP %d", ctx->slot, label, code);
            }
        } else {
            /* Timeout — periodic status poll. */
            if (!ctx->stop_requested) {
                gopro_http_poll_status(ctx);
            }
        }
    }

    ctx->task_handle = NULL;
    vTaskDelete(NULL);
}

/* ---- Driver vtable ------------------------------------------------------- */

static esp_err_t drv_start_recording(void *arg)
{
    gopro_http_ctx_t *ctx = (gopro_http_ctx_t *)arg;
    if (!ctx->cmd_queue) return ESP_ERR_INVALID_STATE;
    gopro_http_cmd_t cmd = HTTP_CMD_START_RECORDING;
    if (xQueueSend(ctx->cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "slot %d: start_recording queue full", ctx->slot);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t drv_stop_recording(void *arg)
{
    gopro_http_ctx_t *ctx = (gopro_http_ctx_t *)arg;
    if (!ctx->cmd_queue) return ESP_ERR_INVALID_STATE;
    gopro_http_cmd_t cmd = HTTP_CMD_STOP_RECORDING;
    if (xQueueSend(ctx->cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "slot %d: stop_recording queue full", ctx->slot);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static camera_recording_status_t drv_get_recording_status(void *arg)
{
    gopro_http_ctx_t *ctx = (gopro_http_ctx_t *)arg;
    xSemaphoreTake(ctx->status_mutex, portMAX_DELAY);
    camera_recording_status_t s = ctx->cached_status;
    xSemaphoreGive(ctx->status_mutex);
    return s;
}

static void drv_teardown(void *arg)
{
    gopro_http_ctx_t *ctx = (gopro_http_ctx_t *)arg;

    ctx->stop_requested = true;

    /* Unblock the task if it is waiting on the queue. */
    if (ctx->cmd_queue) {
        gopro_http_cmd_t dummy = HTTP_CMD_STOP_RECORDING;
        xQueueSend(ctx->cmd_queue, &dummy, 0);
    }

    /* Spin-wait for the worker task to exit (max 1 s). */
    for (int i = 0; i < 100 && ctx->task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (ctx->cmd_queue) {
        vQueueDelete(ctx->cmd_queue);
        ctx->cmd_queue = NULL;
    }
    if (ctx->status_mutex) {
        vSemaphoreDelete(ctx->status_mutex);
        ctx->status_mutex = NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
}

static void drv_update_slot_index(void *arg, int new_slot)
{
    gopro_http_ctx_t *ctx = (gopro_http_ctx_t *)arg;
    ctx->slot = new_slot;
}

static void drv_on_wifi_associated(void *arg, uint32_t ip)
{
    gopro_http_ctx_t *ctx = (gopro_http_ctx_t *)arg;

    if (ctx->task_handle != NULL) {
        /* Stale task still running from a previous association — shouldn't
         * happen in practice, but guard against it. */
        ESP_LOGW(TAG, "slot %d: on_wifi_associated called while worker still running",
                 ctx->slot);
        return;
    }

    ctx->ip              = ip;
    ctx->stop_requested  = false;
    ctx->auth_fail_count = 0;

    /* Reset cached status under mutex. */
    xSemaphoreTake(ctx->status_mutex, portMAX_DELAY);
    ctx->cached_status = CAMERA_RECORDING_UNKNOWN;
    xSemaphoreGive(ctx->status_mutex);

    /* Flush any stale commands from a previous session. */
    if (ctx->cmd_queue) {
        xQueueReset(ctx->cmd_queue);
    }

    /* Read COHN credentials stored by open_gopro_ble/cohn.c. */
    if (!open_gopro_ble_get_cohn_credentials(ctx->slot,
                                              ctx->user, sizeof(ctx->user),
                                              ctx->pass, sizeof(ctx->pass))) {
        ESP_LOGE(TAG, "slot %d: no COHN credentials in NVS — cannot probe", ctx->slot);
        return;
    }

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "gopro_http_%d", ctx->slot);

    BaseType_t ok = xTaskCreate(http_worker_task,
                                task_name,
                                GOPRO_HTTP_TASK_STACK_BYTES / sizeof(StackType_t),
                                ctx,
                                GOPRO_HTTP_TASK_PRIORITY,
                                &ctx->task_handle);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "slot %d: xTaskCreate failed", ctx->slot);
    } else {
        ESP_LOGI(TAG, "slot %d: worker task started (ip=%lu)",
                 ctx->slot, (unsigned long)ip);
    }
}

static void drv_on_wifi_disconnected(void *arg)
{
    gopro_http_ctx_t *ctx = (gopro_http_ctx_t *)arg;
    ctx->stop_requested = true;
    /* Unblock the task so it can exit promptly. */
    if (ctx->cmd_queue) {
        gopro_http_cmd_t dummy = HTTP_CMD_STOP_RECORDING;
        xQueueSend(ctx->cmd_queue, &dummy, 0);
    }
    ESP_LOGI(TAG, "slot %d: wifi disconnected — stopping worker", ctx->slot);
}

static const camera_driver_t k_gopro_http_driver = {
    .start_recording      = drv_start_recording,
    .stop_recording       = drv_stop_recording,
    .get_recording_status = drv_get_recording_status,
    .teardown             = drv_teardown,
    .update_slot_index    = drv_update_slot_index,
    .on_wifi_associated   = drv_on_wifi_associated,
    .on_wifi_disconnected = drv_on_wifi_disconnected,
};

/* ---- Driver registration ------------------------------------------------- */

static bool model_matches(camera_model_t model)
{
    return gopro_model_uses_cohn(model);
}

static void *ctx_create(int slot)
{
    gopro_http_ctx_t *ctx = &s_ctx[slot];
    memset(ctx, 0, sizeof(*ctx));
    ctx->slot          = slot;
    ctx->cached_status = CAMERA_RECORDING_UNKNOWN;
    ctx->status_mutex  = xSemaphoreCreateMutex();
    configASSERT(ctx->status_mutex);
    ctx->cmd_queue = xQueueCreate(GOPRO_HTTP_CMD_QUEUE_DEPTH,
                                   sizeof(gopro_http_cmd_t));
    configASSERT(ctx->cmd_queue);
    return ctx;
}

/* ---- Public API ---------------------------------------------------------- */

/*
 * Called from main's on_station_disassociated callback for every SoftAP
 * departure.  Finds the matching COHN slot (if any) and signals its worker
 * task to stop, mirroring drv_on_wifi_disconnected().  Non-COHN MACs are
 * silently ignored via the model guard.
 */
void open_gopro_http_on_camera_disconnected_by_mac(const uint8_t mac[6])
{
    int slot = camera_manager_find_by_mac(mac);
    if (slot < 0) return;
    if (!gopro_model_uses_cohn(camera_manager_get_model(slot))) return;

    gopro_http_ctx_t *ctx = &s_ctx[slot];
    ctx->stop_requested = true;
    if (ctx->cmd_queue) {
        gopro_http_cmd_t dummy = HTTP_CMD_STOP_RECORDING;
        xQueueSend(ctx->cmd_queue, &dummy, 0);
    }
    ESP_LOGI(TAG, "slot %d: SoftAP disassociation — stopping worker", slot);
}

/* ---- Component init ------------------------------------------------------ */

void open_gopro_http_init(void)
{
    ESP_ERROR_CHECK(
        camera_manager_register_driver(&k_gopro_http_driver,
                                        model_matches,
                                        ctx_create,
                                        true /* requires_ble */));
    ESP_LOGI(TAG, "init OK");
}

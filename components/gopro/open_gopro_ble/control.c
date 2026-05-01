/*
 * control.c — SetDateTime and BLE keepalive timer.
 *
 * SetDateTime (0x0D) is sent best-effort after COHN provisioning and on every
 * UTC sync event.  No retry on failure.
 *
 * The BLE keepalive (0x42 to settings_write GP-0074) is sent every 3 seconds
 * to prevent the camera from auto-sleeping and to maintain the BLE link
 * supervision timeout.  Required even when COHN HTTPS is active because the
 * BLE link supervision timer is independent of WiFi/HTTP activity.
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/control.html
 */

#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "open_gopro_ble_internal.h"

static const char *TAG = "gopro_ble/ctrl";

/* ---- SetDateTime (§15.5) ------------------------------------------------- */

/*
 * SetDateTime TLV packet layout (total payload 14 bytes):
 *
 *   [0]     GPBS general header: 0x0D (length = 13)
 *   [1]     Command ID: 0x0D
 *   [2]     Param ID: 0x01 (date)
 *   [3]     Param len: 0x04
 *   [4-5]   Year (big-endian, e.g. 2026 = 0x07 0xEA)
 *   [6]     Month (1–12)
 *   [7]     Day   (1–31)
 *   [8]     Param ID: 0x02 (time)
 *   [9]     Param len: 0x03
 *   [10]    Hour   (0–23)
 *   [11]    Minute (0–59)
 *   [12]    Second (0–59)
 *
 * Total payload = 1 (cmd) + 2 (date TLV header) + 4 (date value)
 *               + 2 (time TLV header) + 3 (time value) = 12 bytes
 * GPBS header byte: 0x0C (general, len=12)
 */
void gopro_control_set_datetime(gopro_ble_ctx_t *ctx)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm t;
    gmtime_r(&tv.tv_sec, &t);

    int year = t.tm_year + 1900;

    uint8_t pkt[13] = {
        0x0Cu,                            /* GPBS header: general, len=12 */
        GOPRO_CMD_SET_DATE_TIME,          /* command ID */
        GOPRO_DT_PARAM_DATE,              /* param 1: date */
        GOPRO_DT_PARAM_DATE_LEN,          /* param 1 length = 4 */
        (uint8_t)(year >> 8),             /* year high byte */
        (uint8_t)(year & 0xFF),           /* year low byte */
        (uint8_t)(t.tm_mon + 1),          /* month 1-12 */
        (uint8_t)(t.tm_mday),             /* day 1-31 */
        GOPRO_DT_PARAM_TIME,              /* param 2: time */
        GOPRO_DT_PARAM_TIME_LEN,          /* param 2 length = 3 */
        (uint8_t)(t.tm_hour),
        (uint8_t)(t.tm_min),
        (uint8_t)(t.tm_sec),
    };

    ESP_LOGI(TAG, "slot %d: SetDateTime %04d-%02d-%02d %02d:%02d:%02d UTC",
             ctx->slot, year, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                        pkt, sizeof(pkt));
}

/* ---- BLE keepalive (§15.1) ----------------------------------------------- */

static void on_keepalive_timer(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;

    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.settings_write == 0) {
        return;
    }
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.settings_write,
                        k_gopro_keepalive_pkt, sizeof(k_gopro_keepalive_pkt));
}

void gopro_keepalive_start(gopro_ble_ctx_t *ctx)
{
    if (ctx->keepalive_timer) {
        return;  /* already running */
    }

    esp_timer_create_args_t args = {
        .callback        = on_keepalive_timer,
        .arg             = ctx,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "gopro_ka",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &ctx->keepalive_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->keepalive_timer,
                                              GOPRO_KEEPALIVE_PERIOD_MS * 1000ULL));
    ESP_LOGD(TAG, "slot %d: keepalive started", ctx->slot);
}

void gopro_keepalive_stop(gopro_ble_ctx_t *ctx)
{
    if (!ctx->keepalive_timer) {
        return;
    }
    esp_timer_stop(ctx->keepalive_timer);
    esp_timer_delete(ctx->keepalive_timer);
    ctx->keepalive_timer = NULL;
    ESP_LOGD(TAG, "slot %d: keepalive stopped", ctx->slot);
}

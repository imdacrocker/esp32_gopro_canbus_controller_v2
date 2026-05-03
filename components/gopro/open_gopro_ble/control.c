/*
 * control.c — TLV / protobuf control commands and BLE keepalive.
 *
 *   - SetDateTime (0x0D, TLV)
 *   - SetCameraControlStatus(EXTERNAL) (Feature 0xF1, Action 0x69, protobuf)
 *   - SetShutter (0x01, TLV)
 *   - Keepalive (0x42 to settings_write GP-0074, every 3 s)
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/control.html
 */

#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "open_gopro_ble_internal.h"
#include "can_manager.h"

static const char *TAG = "gopro_ble/ctrl";

/* ---- SetDateTime (§15.5) ------------------------------------------------- */

void gopro_control_set_datetime(gopro_ble_ctx_t *ctx)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        return;
    }

    /* Only push time to the camera when UTC has been live-synced this session
     * (CAN GPS frame or web-UI manual set).  An NVS-restored value at boot
     * is "close" but not authoritative — we'd rather let the camera keep its
     * own clock than push a stale value. */
    if (!can_manager_utc_is_session_synced()) {
        ESP_LOGD(TAG, "slot %d: SetDateTime deferred — UTC not session-synced",
                 ctx->slot);
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

/* ---- SetCameraControlStatus(EXTERNAL) ------------------------------------ */

/*
 * Packet layout (5 bytes total):
 *   [0] 0x04             GPBS header (general, len=4)
 *   [1] 0xF1             Feature ID (COMMAND)
 *   [2] 0x69             Action ID  (RequestSetCameraControlStatus)
 *   [3] 0x08             Protobuf field 1 tag (varint)
 *   [4] 0x02             EnumCameraControlStatus.EXTERNAL
 *
 * Response (Feature 0xF1, Action 0xE9, ResponseGeneric) arrives on
 * cmd_resp_notify and is dispatched by query.c.
 */
static const uint8_t k_set_cam_ctrl_pkt[5] = {
    0x04u,
    GOPRO_PROTO_FEATURE_COMMAND,
    GOPRO_CMD_ACTION_SET_CAM_CTRL,
    GOPRO_CAM_CTRL_PB_STATUS_TAG,
    GOPRO_CAM_CTRL_EXTERNAL,
};

int gopro_control_send_set_cam_ctrl(gopro_ble_ctx_t *ctx)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        ESP_LOGW(TAG, "slot %d: SetCameraControlStatus skipped — not connected",
                 ctx->slot);
        return -1;
    }
    ESP_LOGI(TAG, "slot %d: → SetCameraControlStatus(EXTERNAL)", ctx->slot);
    return ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                                k_set_cam_ctrl_pkt, sizeof(k_set_cam_ctrl_pkt));
}

/* ---- SetShutter (start/stop recording) ----------------------------------- */

int gopro_control_send_shutter(gopro_ble_ctx_t *ctx, bool on)
{
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.cmd_write == 0) {
        ESP_LOGW(TAG, "slot %d: SetShutter skipped — not connected", ctx->slot);
        return -1;
    }

    /* TLV: [GPBS hdr=3, cmd=0x01, param_len=0x01, value=0|1] */
    uint8_t pkt[4] = {
        0x03u,                       /* GPBS header: general, len=3 */
        GOPRO_CMD_SET_SHUTTER,
        0x01u,                       /* param length = 1 */
        on ? 0x01u : 0x00u,
    };

    ESP_LOGI(TAG, "slot %d: → SetShutter(%s)", ctx->slot, on ? "ON" : "OFF");
    return ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
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

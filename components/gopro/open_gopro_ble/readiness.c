/*
 * readiness.c — Post-GATT connection sequence (V1-style).
 *
 *   1. GetHardwareInfo poll until status=0  (up to GOPRO_READINESS_RETRY_MAX)
 *   2. SetCameraControlStatus(EXTERNAL)     (ResponseGeneric or 3 s timeout)
 *   3. camera_manager_on_camera_ready()     → wifi_status = WIFI_CAM_READY
 *   4. SetDateTime (best-effort, deferred if UTC not session-synced)
 *   5. Start the 5 s GetStatusValue poll
 *
 * The connection sequence proceeds even if SetCameraControlStatus times out —
 * a silent camera should not permanently stall setup.
 *
 * All callbacks run on the NimBLE host task or the esp_timer task.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "open_gopro_ble_internal.h"
#include "gopro_model.h"
#include "can_manager.h"

static const char *TAG = "gopro_ble/ready";

/* GetHardwareInfo packet: GPBS general header len=1, cmd=0x3C */
static const uint8_t k_get_hwinfo_pkt[2] = { 0x01u, GOPRO_CMD_GET_HARDWARE_INFO };

/* ---- GetHardwareInfo response payload parser ----------------------------- */

/* Consume one LV string field; returns bytes consumed or 0 on error. */
static int lv_take_string(const uint8_t *data, uint16_t remaining,
                           char *out, size_t out_size)
{
    if (remaining < 1) {
        return 0;
    }
    uint8_t flen = data[0];
    if ((uint16_t)(1 + flen) > remaining) {
        return 0;
    }
    size_t copy = (flen < out_size - 1) ? flen : out_size - 1;
    memcpy(out, &data[1], copy);
    out[copy] = '\0';
    return 1 + flen;
}

/* Consume one LV uint field (1–4 bytes, big-endian); returns bytes consumed or 0. */
static int lv_take_uint(const uint8_t *data, uint16_t remaining, uint32_t *out)
{
    if (remaining < 1) {
        return 0;
    }
    uint8_t flen = data[0];
    if ((uint16_t)(1 + flen) > remaining || flen == 0 || flen > 4) {
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < flen; i++) {
        v = (v << 8) | data[1 + i];
    }
    *out = v;
    return 1 + flen;
}

static uint32_t parse_and_log_hw_info(int slot, const uint8_t *body, uint16_t len)
{
    char     model_name[32] = {0};
    char     deprecated[32] = {0};
    char     firmware[32]   = {0};
    char     serial[24]     = {0};
    char     ap_ssid[24]    = {0};
    uint32_t model_num      = 0;
    uint16_t i              = 0;
    int      n;

    n = lv_take_uint(&body[i], len - i, &model_num);
    if (n == 0) goto out;
    i += (uint16_t)n;

    n = lv_take_string(&body[i], len - i, model_name, sizeof(model_name));
    if (n == 0) goto out;
    i += (uint16_t)n;

    n = lv_take_string(&body[i], len - i, deprecated, sizeof(deprecated));
    if (n == 0) goto out;
    i += (uint16_t)n;

    n = lv_take_string(&body[i], len - i, firmware, sizeof(firmware));
    if (n == 0) goto out;
    i += (uint16_t)n;

    n = lv_take_string(&body[i], len - i, serial, sizeof(serial));
    if (n == 0) goto out;
    i += (uint16_t)n;

    n = lv_take_string(&body[i], len - i, ap_ssid, sizeof(ap_ssid));
    if (n == 0) goto out;
    i += (uint16_t)n;

    char ap_mac[24] = {0};
    if (i < len) {
        uint8_t mlen = body[i++];
        if ((uint16_t)(i + mlen) <= len && mlen <= 8) {
            char *p = ap_mac;
            for (int b = 0; b < mlen && (p - ap_mac) + 4 < (int)sizeof(ap_mac); b++) {
                p += sprintf(p, "%s%02X", b == 0 ? "" : ":", body[i + b]);
            }
        }
    }

out:
    ESP_LOGI(TAG, "slot %d GetHardwareInfo: model=%u (%s)  fw=%s  sn=%s  ssid=%s  mac=%s",
             slot, (unsigned)model_num, model_name, firmware, serial, ap_ssid, ap_mac);
    return model_num;
}

/* ---- Stage 3: camera fully ready ----------------------------------------- */

static void complete_connection_sequence(gopro_ble_ctx_t *ctx)
{
    int slot = ctx->slot;
    ESP_LOGI(TAG, "slot %d: camera ready — completing connection sequence", slot);

    camera_manager_on_camera_ready(slot);

    /* Date/time best-effort — gopro_control_set_datetime() returns silently
     * when UTC is not session-synced.  If deferred, the flag will trigger a
     * resend from open_gopro_ble_sync_time_all() once UTC arrives. */
    uint64_t utc_ms;
    bool utc_ok = can_manager_utc_is_session_synced();
    (void)can_manager_get_utc_ms(&utc_ms);

    if (utc_ok) {
        gopro_control_set_datetime(ctx);
    } else {
        ESP_LOGI(TAG, "slot %d: SetDateTime deferred — UTC not session-synced",
                 slot);
        ctx->datetime_pending_utc = true;
    }

    gopro_status_poll_start(ctx);
}

/* ---- Stage 2: SetCameraControlStatus(EXTERNAL) handshake ----------------- */

static void cam_ctrl_timeout_cb(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;

    if (!ctx->cam_ctrl_pending || ctx->conn_handle == GOPRO_CONN_NONE) {
        return;
    }
    ESP_LOGW(TAG, "slot %d: SetCameraControlStatus timed out — proceeding anyway",
             ctx->slot);
    ctx->cam_ctrl_pending = false;
    complete_connection_sequence(ctx);
}

void gopro_readiness_handle_cam_ctrl_acked(gopro_ble_ctx_t *ctx, uint8_t result)
{
    if (!ctx->cam_ctrl_pending) {
        return;
    }

    if (ctx->cam_ctrl_timer) {
        esp_timer_stop(ctx->cam_ctrl_timer);
    }
    ctx->cam_ctrl_pending = false;

    if (result == GOPRO_RESP_GENERIC_SUCCESS) {
        ESP_LOGI(TAG, "slot %d: SetCameraControlStatus → CAMERA_EXTERNAL_CONTROL",
                 ctx->slot);
    } else {
        ESP_LOGW(TAG, "slot %d: SetCameraControlStatus rejected (result=%u) — proceeding",
                 ctx->slot, result);
    }

    complete_connection_sequence(ctx);
}

static void send_set_cam_ctrl(gopro_ble_ctx_t *ctx)
{
    /* Lazy timer creation. */
    if (ctx->cam_ctrl_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback        = cam_ctrl_timeout_cb,
            .arg             = ctx,
            .dispatch_method = ESP_TIMER_TASK,
            .name            = "gopro_cam_ctrl",
        };
        if (esp_timer_create(&args, &ctx->cam_ctrl_timer) != ESP_OK) {
            ESP_LOGW(TAG, "slot %d: cam_ctrl timer create failed — skipping handshake",
                     ctx->slot);
            complete_connection_sequence(ctx);
            return;
        }
    }

    ctx->cam_ctrl_pending = true;
    int rc = gopro_control_send_set_cam_ctrl(ctx);
    if (rc != 0) {
        ESP_LOGW(TAG, "slot %d: SetCameraControlStatus send failed — skipping handshake",
                 ctx->slot);
        ctx->cam_ctrl_pending = false;
        complete_connection_sequence(ctx);
        return;
    }
    esp_timer_start_once(ctx->cam_ctrl_timer,
                         (uint64_t)GOPRO_CAM_CTRL_TIMEOUT_MS * 1000ULL);
}

/* ---- Stage 1: GetHardwareInfo readiness poll ----------------------------- */

static void gopro_on_hw_info_ok(gopro_ble_ctx_t *ctx, uint32_t model_num)
{
    int slot = ctx->slot;
    camera_model_t model = (camera_model_t)model_num;
    ESP_LOGI(TAG, "slot %d: hardware ready, model=%u", slot, (unsigned)model_num);

    camera_manager_set_model(slot, model);
    esp_err_t save_err = camera_manager_save_slot(slot);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "slot %d: NVS save after pairing failed: %s",
                 slot, esp_err_to_name(save_err));
    }

    camera_manager_on_ble_ready(slot);

    /* Keepalive must run as long as we hold the BLE connection — start it
     * before the SetCameraControlStatus handshake. */
    gopro_keepalive_start(ctx);

    /* Tell the camera the initial pairing flow is complete — clears the
     * on-screen pairing prompt on supported models (Hero11 Mini / Hero12 /
     * Hero13 / Max 2 / Lit Hero).  Fire-and-forget; older models without a
     * Network Management characteristic skip this internally. */
    gopro_control_send_pairing_finish(ctx);

    send_set_cam_ctrl(ctx);
}

void gopro_readiness_on_response(gopro_ble_ctx_t *ctx,
                                  const uint8_t *data, uint16_t len);

void gopro_readiness_on_response(gopro_ble_ctx_t *ctx,
                                  const uint8_t *data, uint16_t len)
{
    if (!ctx->readiness_polling) {
        return;
    }

    if (len < GOPRO_RESP_HDR_LEN) {
        ESP_LOGW(TAG, "slot %d: HwInfo response too short (%d)", ctx->slot, len);
        return;
    }
    if (data[GOPRO_RESP_STATUS_IDX] != GOPRO_RESP_STATUS_OK) {
        ESP_LOGW(TAG, "slot %d: HwInfo status=0x%02x, retry %d/%d",
                 ctx->slot, data[GOPRO_RESP_STATUS_IDX],
                 ctx->readiness_retry_count, GOPRO_READINESS_RETRY_MAX);
        return;  /* timer will fire and retry */
    }

    /* Success — stop poll timer before any further work. */
    ctx->readiness_polling = false;
    if (ctx->readiness_timer) {
        esp_timer_stop(ctx->readiness_timer);
    }

    uint32_t model_num = parse_and_log_hw_info(ctx->slot,
                                                data + GOPRO_RESP_HDR_LEN,
                                                len - GOPRO_RESP_HDR_LEN);
    gopro_on_hw_info_ok(ctx, model_num);
}

/* ---- Readiness retry timer ----------------------------------------------- */

static void on_readiness_timer(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;

    if (!ctx->readiness_polling || ctx->conn_handle == GOPRO_CONN_NONE) {
        return;
    }

    ctx->readiness_retry_count++;
    if (ctx->readiness_retry_count > GOPRO_READINESS_RETRY_MAX) {
        ESP_LOGW(TAG, "slot %d: readiness timeout, disconnecting", ctx->slot);
        ctx->readiness_polling = false;
        ble_gap_terminate(ctx->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    ESP_LOGD(TAG, "slot %d: readiness retry %d", ctx->slot,
             ctx->readiness_retry_count);

    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                        k_get_hwinfo_pkt, sizeof(k_get_hwinfo_pkt));

    esp_timer_start_once(ctx->readiness_timer,
                         GOPRO_READINESS_RETRY_MS * 1000ULL);
}

/* ---- Public API ---------------------------------------------------------- */

void gopro_readiness_start(gopro_ble_ctx_t *ctx)
{
    ctx->readiness_retry_count = 0;
    ctx->readiness_polling     = true;

    if (!ctx->readiness_timer) {
        esp_timer_create_args_t args = {
            .callback        = on_readiness_timer,
            .arg             = ctx,
            .dispatch_method = ESP_TIMER_TASK,
            .name            = "gopro_ready",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &ctx->readiness_timer));
    }

    /* Send first GetHardwareInfo immediately. */
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write,
                        k_get_hwinfo_pkt, sizeof(k_get_hwinfo_pkt));

    esp_timer_start_once(ctx->readiness_timer,
                         GOPRO_READINESS_RETRY_MS * 1000ULL);
}

void gopro_readiness_cancel(gopro_ble_ctx_t *ctx)
{
    ctx->readiness_polling = false;
    if (ctx->readiness_timer) {
        esp_timer_stop(ctx->readiness_timer);
        esp_timer_delete(ctx->readiness_timer);
        ctx->readiness_timer = NULL;
    }
    ctx->cam_ctrl_pending = false;
    if (ctx->cam_ctrl_timer) {
        esp_timer_stop(ctx->cam_ctrl_timer);
        esp_timer_delete(ctx->cam_ctrl_timer);
        ctx->cam_ctrl_timer = NULL;
    }
}

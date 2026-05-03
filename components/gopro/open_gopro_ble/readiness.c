/*
 * readiness.c — GetHardwareInfo poll and camera model extraction.
 *
 * After GATT setup is complete, we repeatedly send GetHardwareInfo (0x3C) to
 * verify the camera's GoPro stack is up and to read the model number.
 * On success, gopro_on_camera_ready() branches into provisioning or ready state.
 * On failure after GOPRO_READINESS_RETRY_MAX attempts the connection is dropped.
 *
 * All code runs on the NimBLE host task except the esp_timer callback, which
 * is rescheduled on the timer task and posts work back via ble_core_gatt_write().
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

/*
 * The GetHardwareInfo response body is a sequence of positional length-value
 * fields (NOT type-length-value).  Each field is [len (1B), value (len B)],
 * and fields appear in a fixed order:
 *
 *   1) model number   (uint32_t, big-endian)
 *   2) model name     (string)
 *   3) deprecated     (string, ignored)
 *   4) firmware       (string)
 *   5) serial number  (string)
 *   6) AP SSID        (string)
 *   7) AP MAC address (6 raw bytes)
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/query.html#get-hardware-info
 */

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

/*
 * Parse the GetHardwareInfo response body, log the human-readable fields,
 * and return the model number (0 if it could not be parsed).
 */
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

    /* AP MAC: 1-byte length followed by raw bytes — format as XX:XX:... */
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

/* ---- gopro_on_camera_ready (§15.5) --------------------------------------- */

void gopro_on_camera_ready(gopro_ble_ctx_t *ctx, uint32_t model_num)
{
    camera_model_t model = (camera_model_t)model_num;
    ESP_LOGI(TAG, "slot %d: camera ready model=%u", ctx->slot, model_num);

    camera_manager_set_model(ctx->slot, model);

    esp_err_t save_err = camera_manager_save_slot(ctx->slot);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "slot %d: NVS save after pairing failed: %s",
                 ctx->slot, esp_err_to_name(save_err));
    }

    /* Check NVS for existing COHN credentials. */
    char user[32];
    char pass[64];
    extern bool gopro_cohn_load(int slot, char *user, size_t ulen,
                                char *pass, size_t plen);
    bool have_creds = gopro_cohn_load(ctx->slot, user, sizeof(user),
                                      pass, sizeof(pass));

    if (have_creds) {
        ESP_LOGI(TAG, "slot %d: COHN credentials present — camera ready", ctx->slot);
        /* Send datetime on best-effort basis if UTC is available. */
        gopro_control_set_datetime(ctx);
        camera_manager_on_ble_ready(ctx->slot);
        /* Cached-credential path: transition wifi_status → CONNECTED so
         * on_station_ip can dispatch the HTTP probe (or, if last_ip is
         * already populated from an earlier DHCP event during this boot,
         * dispatch the probe now).  Without this, the COHN driver never
         * starts after a reboot. */
        camera_manager_set_camera_ready(ctx->slot, true);
        gopro_keepalive_start(ctx);
    } else {
        /* Need to provision COHN. */
        gopro_control_set_datetime(ctx);
        /* Need a UTC anchor for COHN cert generation.  An NVS-restored value
         * is good enough here (cert validity windows are months).  Live sync
         * is only required for pushing time to the camera, gated separately
         * inside gopro_control_set_datetime(). */
        uint64_t utc_ms;
        bool utc_ok = can_manager_get_utc_ms(&utc_ms);

        if (utc_ok) {
            gopro_keepalive_start(ctx);
            gopro_cohn_provision(ctx);
        } else {
            ESP_LOGI(TAG, "slot %d: UTC not available, deferring COHN provision",
                     ctx->slot);
            ctx->cohn_pending_utc = true;
            gopro_keepalive_start(ctx);
        }
    }
}

/* ---- Dispatch from query.c ----------------------------------------------- */

/*
 * Called by query.c when a complete GetHardwareInfo response arrives on
 * cmd_resp_notify (GP-0073).
 */
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
    esp_timer_stop(ctx->readiness_timer);

    /* Parse and log the LV body that follows the [cmd_id][status] header. */
    uint32_t model_num = parse_and_log_hw_info(ctx->slot,
                                                data + GOPRO_RESP_HDR_LEN,
                                                len - GOPRO_RESP_HDR_LEN);
    gopro_on_camera_ready(ctx, model_num);
}

/* ---- Timer callback ------------------------------------------------------ */

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
}

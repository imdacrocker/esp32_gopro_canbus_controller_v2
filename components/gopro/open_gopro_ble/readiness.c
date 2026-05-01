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

#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "open_gopro_ble_internal.h"
#include "gopro_model.h"

static const char *TAG = "gopro_ble/ready";

/* GetHardwareInfo packet: GPBS general header len=1, cmd=0x3C */
static const uint8_t k_get_hwinfo_pkt[2] = { 0x01u, GOPRO_CMD_GET_HARDWARE_INFO };

/* ---- TLV parser ---------------------------------------------------------- */

/*
 * Walk a TLV-encoded parameter block starting at data[offset].
 * Returns the uint32_t value of the first parameter whose ID matches target_id,
 * or 0 if not found.
 *
 * TLV format per parameter: [id (1B), len (1B), value (len B)]
 */
static uint32_t tlv_find_u32(const uint8_t *data, uint16_t len,
                              uint8_t target_id)
{
    uint16_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t  id  = data[pos];
        uint8_t  plen = data[pos + 1];
        pos += 2;
        if (pos + plen > len) {
            break;
        }
        if (id == target_id && plen >= 1 && plen <= 4) {
            uint32_t val = 0;
            for (int i = 0; i < plen; i++) {
                val = (val << 8) | data[pos + i];
            }
            return val;
        }
        pos += plen;
    }
    return 0;
}

/* ---- gopro_on_camera_ready (§15.5) --------------------------------------- */

void gopro_on_camera_ready(gopro_ble_ctx_t *ctx, uint32_t model_num)
{
    camera_model_t model = (camera_model_t)model_num;
    ESP_LOGI(TAG, "slot %d: camera ready model=%u", ctx->slot, model_num);

    camera_manager_set_model(ctx->slot, model);

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
        gopro_keepalive_start(ctx);
    } else {
        /* Need to provision COHN. */
        gopro_control_set_datetime(ctx);
        /* Is UTC available?  Treat any timestamp after 2020-01-01 as synced. */
        bool utc_ok = (time(NULL) > 1577836800LL);

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

    /* Parse model number from TLV body (starts after 3-byte response header). */
    uint32_t model_num = tlv_find_u32(data + GOPRO_RESP_HDR_LEN,
                                       len - GOPRO_RESP_HDR_LEN,
                                       GOPRO_HWINFO_PARAM_MODEL_NUM);
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

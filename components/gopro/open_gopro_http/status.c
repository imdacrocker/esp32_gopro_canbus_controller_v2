/*
 * status.c — Camera state polling and probe logic.
 *
 * gopro_http_probe()      — called once by the worker task at startup.
 * gopro_http_poll_status() — called periodically by the worker task.
 *
 * Both parse /gopro/camera/state and update ctx->cached_status.
 * On 401, auth_fail_count is incremented; at the threshold the BLE
 * re-provisioning path is triggered via open_gopro_ble_reprovision().
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "open_gopro_http_internal.h"
#include "camera_manager.h"
#include "open_gopro_ble.h"

static const char *TAG = "gopro_http/status";

/* ---- JSON parsing -------------------------------------------------------- */

/*
 * Parse /gopro/camera/state response body without a JSON library.
 *
 * Response shape: { "status": { "8": <int>, ... }, "settings": {...} }
 *
 * Strategy:
 *   1. Locate the "status" key.
 *   2. Find the opening '{' of the status object.
 *   3. Within that object, locate the target key (e.g. "8").
 *   4. Parse the integer that follows the ':'.
 *
 * Returns CAMERA_RECORDING_ACTIVE, CAMERA_RECORDING_IDLE, or
 * CAMERA_RECORDING_UNKNOWN on any parse failure.
 */
static camera_recording_status_t parse_state(const char *body)
{
    /* 1. Find the "status" key. */
    const char *status_token = "\"" GOPRO_HTTP_STATUS_OBJ_KEY "\"";
    const char *p = strstr(body, status_token);
    if (!p) return CAMERA_RECORDING_UNKNOWN;
    p += strlen(status_token);

    /* 2. Find the '{' that opens the status object. */
    p = strchr(p, '{');
    if (!p) return CAMERA_RECORDING_UNKNOWN;
    const char *obj_start = p + 1;

    /* 3. Find the closing '}' of the status object (one level, not nested). */
    const char *obj_end = strchr(obj_start, '}');
    if (!obj_end) return CAMERA_RECORDING_UNKNOWN;

    /* 4. Locate "8": within the object bounds. */
    const char *needle = "\"" GOPRO_HTTP_STATUS_ENCODING_ID "\":";
    const char *key = obj_start;
    while (key < obj_end) {
        key = strstr(key, needle);
        if (!key || key >= obj_end) return CAMERA_RECORDING_UNKNOWN;
        /* Verify the character before the opening quote is not part of a
         * longer key (e.g. "18" or "108").  JSON keys are quoted, so the
         * preceding char must be '"' (end of previous value) or '{' or ','. */
        if (key > obj_start) {
            /* Walk back past whitespace to find the delimiter. */
            const char *prev = key - 1;
            while (prev >= obj_start && (*prev == ' ' || *prev == '\n' || *prev == '\r' || *prev == '\t')) {
                prev--;
            }
            /* Valid key boundaries: '{', ',', or we're at the start. */
            if (prev >= obj_start && *prev != '{' && *prev != ',') {
                key++;  /* false match — skip and keep searching */
                continue;
            }
        }
        break;
    }
    if (!key || key >= obj_end) return CAMERA_RECORDING_UNKNOWN;

    /* Advance past the needle to the value. */
    key += strlen(needle);
    while (*key == ' ') key++;

    /* The value is a decimal integer: 0 = idle, non-zero = recording. */
    if (*key == '0') return CAMERA_RECORDING_IDLE;
    if (*key >= '1' && *key <= '9') return CAMERA_RECORDING_ACTIVE;
    return CAMERA_RECORDING_UNKNOWN;
}

/* ---- Shared helper ------------------------------------------------------- */

static void update_cached_status(gopro_http_ctx_t *ctx,
                                  camera_recording_status_t s)
{
    xSemaphoreTake(ctx->status_mutex, portMAX_DELAY);
    ctx->cached_status = s;
    xSemaphoreGive(ctx->status_mutex);
}

/*
 * Fetch /gopro/camera/state and update cached_status.
 * Returns the HTTP status code, or negative on transport failure.
 */
static int fetch_and_update(gopro_http_ctx_t *ctx)
{
    char body[GOPRO_HTTP_RESP_MAX];
    int code = gopro_http_get(ctx, GOPRO_HTTP_PATH_STATE, body, sizeof(body));

    if (code == 200) {
        camera_recording_status_t s = parse_state(body);
        update_cached_status(ctx, s);
        ctx->auth_fail_count = 0;
    } else if (code == 401) {
        ctx->auth_fail_count++;
        ESP_LOGW(TAG, "slot %d: 401 (%d/%d)",
                 ctx->slot, ctx->auth_fail_count, GOPRO_HTTP_AUTH_FAIL_MAX);
        if (ctx->auth_fail_count >= GOPRO_HTTP_AUTH_FAIL_MAX) {
            ESP_LOGE(TAG, "slot %d: auth fail threshold — triggering re-provisioning",
                     ctx->slot);
            ctx->auth_fail_count = 0;
            open_gopro_ble_reprovision(ctx->slot);
        }
    }

    return code;
}

/* ---- Public API ---------------------------------------------------------- */

void gopro_http_probe(gopro_http_ctx_t *ctx)
{
    for (uint8_t attempt = 0; attempt < GOPRO_HTTP_PROBE_RETRIES; attempt++) {
        if (ctx->stop_requested) {
            return;
        }

        ESP_LOGI(TAG, "slot %d: probe attempt %d/%d",
                 ctx->slot, attempt + 1, GOPRO_HTTP_PROBE_RETRIES);

        int code = fetch_and_update(ctx);
        if (code == 200) {
            ESP_LOGI(TAG, "slot %d: probe OK — wifi ready", ctx->slot);
            camera_manager_on_wifi_connected(ctx->slot, ctx->ip);
            return;
        }

        if (attempt + 1 < GOPRO_HTTP_PROBE_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(GOPRO_HTTP_PROBE_RETRY_MS));
        }
    }

    ESP_LOGE(TAG, "slot %d: probe failed after %d attempts",
             ctx->slot, GOPRO_HTTP_PROBE_RETRIES);
    /* wifi_status stays WIFI_CAM_CONNECTED; on_wifi_associated will retry
     * when the camera next disconnects and reconnects to the SoftAP. */
}

void gopro_http_poll_status(gopro_http_ctx_t *ctx)
{
    int code = fetch_and_update(ctx);
    if (code > 0 && code != 200 && code != 401) {
        ESP_LOGW(TAG, "slot %d: poll returned HTTP %d", ctx->slot, code);
    } else if (code < 0) {
        ESP_LOGW(TAG, "slot %d: poll transport error", ctx->slot);
        update_cached_status(ctx, CAMERA_RECORDING_UNKNOWN);
    }
}

/*
 * status.c — Periodic HTTP status poll and JSON recording-state parser.
 *
 * rc_handle_status_poll_all() is called every 5 s from the work task when the
 * global status-poll timer fires.  It issues GET /gp/gpControl/status to every
 * wifi_ready slot and updates ctx->recording_status from the response.
 *
 * §17.8 of camera_manager_design.md.
 */

#include <string.h>
#include "esp_log.h"
#include "camera_manager.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/status";

/* ---- JSON parser --------------------------------------------------------- */

/*
 * Parse the recording state from a /gp/gpControl/status response body.
 *
 * Response shape: { "status": { "8": <int>, ... }, "settings": {...} }
 *
 * Strategy:
 *   1. Find the "status" key.
 *   2. Find the opening '{' of its object.
 *   3. Find "8": inside that object.
 *   4. Parse the integer value (0 = idle, non-zero = recording).
 *
 * Returns CAMERA_RECORDING_ACTIVE, CAMERA_RECORDING_IDLE, or
 * CAMERA_RECORDING_UNKNOWN on parse failure.
 */
static camera_recording_status_t parse_recording_status(const char *body)
{
    /* 1. Locate the "status" key. */
    const char *status_token = "\"" RC_STATUS_JSON_OBJ "\"";
    const char *p = strstr(body, status_token);
    if (!p) return CAMERA_RECORDING_UNKNOWN;
    p += strlen(status_token);

    /* 2. Find the '{' that opens the status object. */
    p = strchr(p, '{');
    if (!p) return CAMERA_RECORDING_UNKNOWN;
    const char *obj_start = p + 1;

    /* 3. Find the matching '}' at the same nesting level. */
    const char *obj_end = strchr(obj_start, '}');
    if (!obj_end) return CAMERA_RECORDING_UNKNOWN;

    /* 4. Locate "8": inside the object bounds, taking care not to match "18"
     *    or "108" — the character before the key's opening quote must be a
     *    JSON separator ('{', ',') or whitespace-preceded separator. */
    const char *needle = "\"" RC_STATUS_JSON_ENCODING "\":";
    const char *key    = obj_start;
    while (key < obj_end) {
        key = strstr(key, needle);
        if (!key || key >= obj_end) return CAMERA_RECORDING_UNKNOWN;

        /* Walk backwards past whitespace to find the preceding delimiter. */
        const char *prev = key - 1;
        while (prev >= obj_start &&
               (*prev == ' ' || *prev == '\n' || *prev == '\r' || *prev == '\t')) {
            prev--;
        }
        if (prev >= obj_start && *prev != '{' && *prev != ',') {
            key++; /* False match (e.g. "18") — keep searching. */
            continue;
        }
        break;
    }
    if (!key || key >= obj_end) return CAMERA_RECORDING_UNKNOWN;

    key += strlen(needle);
    while (*key == ' ') key++;

    if (*key == '0') return CAMERA_RECORDING_IDLE;
    if (*key >= '1' && *key <= '9') return CAMERA_RECORDING_ACTIVE;
    return CAMERA_RECORDING_UNKNOWN;
}

/* ---- Poll handler -------------------------------------------------------- */

/*
 * Called every 5 s from the work task.  Polls all wifi_ready slots
 * sequentially and updates each slot's recording_status cache.
 */
void rc_handle_status_poll_all(void)
{
    /* Static buffer — avoids 4 KB stack allocation per call. */
    static char s_body[RC_HTTP_STATUS_RESP_MAX];

    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        gopro_wifi_rc_ctx_t *ctx = &s_ctx[i];
        if (!ctx->wifi_ready) continue;

        int code = rc_http_get(ctx->last_ip, RC_HTTP_PATH_STATUS,
                               s_body, sizeof(s_body));
        if (code == 200) {
            ctx->recording_status = parse_recording_status(s_body);
            ESP_LOGD(TAG, "slot %d: status poll → %s", i,
                     ctx->recording_status == CAMERA_RECORDING_ACTIVE  ? "recording" :
                     ctx->recording_status == CAMERA_RECORDING_IDLE    ? "idle"      :
                                                                          "unknown");
        } else if (code > 0) {
            ESP_LOGW(TAG, "slot %d: status poll → HTTP %d", i, code);
            ctx->recording_status = CAMERA_RECORDING_UNKNOWN;
        } else {
            ESP_LOGW(TAG, "slot %d: status poll — transport error", i);
            ctx->recording_status = CAMERA_RECORDING_UNKNOWN;
        }
    }
}

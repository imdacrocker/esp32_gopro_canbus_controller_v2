/*
 * command.c — Shutter task (UDP), minimal HTTP/1.0 GET helper used only by
 * identify + datetime, JSON substring extractor for the identify response,
 * and the date/time setter.
 *
 * §17.2.5 (identify), §17.2.6 (date/time), §17.7 (shutter), §17.8 (sync time).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_manager.h"
#include "can_manager.h"
#include "gopro_model.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/cmd";

/* ---- Plain HTTP/1.0 GET helper ------------------------------------------- */

/*
 * Minimal HTTP/1.0 GET via raw lwIP BSD sockets.  Used only for the identify
 * probe and date/time set; not on the recurring control path.  HTTP/1.0 closes
 * the connection after the response, so we read until EOF (or buf_len-1).
 *
 * Returns the HTTP status code (e.g. 200) on success, or -1 on transport
 * failure (connect / send / empty recv).  resp_buf, if non-NULL, receives the
 * body (after stripping the response headers) NUL-terminated; silently
 * truncated if buf_len-1 is exceeded.
 */
int rc_http_get(uint32_t ip, const char *path, int timeout_ms,
                char *resp_buf, size_t buf_len)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed: %d", errno);
        return -1;
    }

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RC_HTTP_PORT),
        .sin_addr.s_addr = ip,
    };
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* errno=104 (ECONNRESET) is the expected outcome on Hero3-class
         * cameras — they don't run an HTTP server on their STA interface.
         * Logged at DEBUG so it doesn't drown the legacy-camera path. */
        ESP_LOGD(TAG, "connect() failed: errno=%d", errno);
        close(sock);
        return -1;
    }

    /* Format IP for the Host header.  ip is in network byte order. */
    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));

    /* Minimal HTTP/1.0 request — no extra headers that confuse Hero4. */
    char req[256];
    int  req_len = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n",
                            path, ip_str);
    if (send(sock, req, req_len, 0) < 0) {
        ESP_LOGD(TAG, "send() failed: %d", errno);
        close(sock);
        return -1;
    }

    /* If the caller didn't supply a buffer, drain into a tiny scratch just
     * long enough to parse the status line. */
    char  scratch[RC_HTTP_CMD_RESP_MAX];
    char *rbuf;
    int   rbuf_size;
    if (resp_buf && buf_len > 0) {
        rbuf      = resp_buf;
        rbuf_size = (int)buf_len;
    } else {
        rbuf      = scratch;
        rbuf_size = (int)sizeof(scratch);
    }

    int total = 0;
    int n;
    while ((n = recv(sock, rbuf + total, rbuf_size - 1 - total, 0)) > 0) {
        total += n;
        if (total >= rbuf_size - 1) break;
    }
    rbuf[total] = '\0';
    close(sock);

    if (total == 0) return -1;

    /* Parse "HTTP/1.x NNN ..." status line. */
    int status = -1;
    if (strncmp(rbuf, "HTTP/", 5) == 0) {
        const char *sp = strchr(rbuf, ' ');
        if (sp) status = atoi(sp + 1);
    }

    /* If the caller asked for the body, strip headers in-place. */
    if (resp_buf && buf_len > 0) {
        char *body = strstr(rbuf, "\r\n\r\n");
        if (body) {
            body += 4;
            size_t body_len = (size_t)(rbuf + total - body);
            memmove(rbuf, body, body_len + 1);  /* +1 to copy NUL */
        } else {
            rbuf[0] = '\0';
        }
    }

    return status;
}

/* ---- Identify-response JSON substring extractor (§17.2.5) ---------------- */

/*
 * Find the first occurrence of "<key>": within `body` and copy the following
 * string literal (up to the closing ") into out.  Skips whitespace between
 * the colon and the opening quote.  Returns true on success.
 */
static bool extract_json_string(const char *body, const char *key,
                                 char *out, size_t out_size)
{
    if (!body || !key || !out || out_size == 0) return false;

    char pat[32];
    int  pn = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pn < 0 || pn >= (int)sizeof(pat)) return false;

    const char *p = strstr(body, pat);
    if (!p) return false;
    p += pn;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/*
 * Same shape as extract_json_string but for an integer literal.
 * Returns true on success and writes the parsed value to *out.
 */
static bool extract_json_int(const char *body, const char *key, int *out)
{
    if (!body || !key || !out) return false;

    char pat[32];
    int  pn = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pn < 0 || pn >= (int)sizeof(pat)) return false;

    const char *p = strstr(body, pat);
    if (!p) return false;
    p += pn;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return false;

    *out = (int)strtol(p, NULL, 10);
    return true;
}

bool rc_parse_identify_json(const char *body,
                             char *name_out, size_t name_size,
                             int  *model_number_out,
                             char *fw_out,   size_t fw_size)
{
    if (name_out && name_size) name_out[0] = '\0';
    if (fw_out && fw_size)     fw_out[0]   = '\0';
    if (model_number_out)      *model_number_out = -1;

    if (!body) return false;

    /* Narrow the search to the "info" object so that we don't accidentally
     * match identically-named fields elsewhere in the JSON.  `services`
     * objects on some firmware revs contain a stray `"name"` field. */
    const char *info = strstr(body, "\"info\"");
    const char *scope = info ? info : body;

    bool got_name = false;
    if (name_out && name_size) {
        got_name = extract_json_string(scope, "model_name", name_out, name_size);
    }
    if (model_number_out) {
        extract_json_int(scope, "model_number", model_number_out);
    }
    if (fw_out && fw_size) {
        extract_json_string(scope, "firmware_version", fw_out, fw_size);
    }

    return got_name;
}

/* ---- Shutter task (priority 7) ------------------------------------------- */

/*
 * Loop forever on s_shutter_queue.  For each command, send one UDP `SH`
 * datagram per wifi_ready slot.  Sequential dispatch across 4 cameras now
 * finishes inside one scheduler tick (was ~200 ms HTTP).
 */
void rc_shutter_task(void *arg)
{
    (void)arg;
    rc_shutter_cmd_t cmd;
    while (1) {
        xQueueReceive(s_shutter_queue, &cmd, portMAX_DELAY);
        bool start = (cmd == RC_SHUTTER_START);

        for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
            if (!s_ctx[i].wifi_ready) continue;
            rc_send_sh(s_ctx[i].last_ip, start);
            ESP_LOGD(TAG, "slot %d: shutter %s sent", i, start ? "start" : "stop");
        }
    }
}

/* ---- Date/time sync (§17.2.6) -------------------------------------------- */

/*
 * Send the current local time to a single slot's camera over HTTP/1.0.
 *
 * Gated by:
 *   - gopro_model_supports_http_datetime() — false for HERO_LEGACY_RC, so
 *     Hero3-class slots silently skip without a noisy log.
 *   - can_manager_utc_is_session_synced() — true only after a live UTC source
 *     has won this session (CAN GPS frame or web-UI manual set).  An NVS-
 *     restored UTC at boot does NOT unlock this path; we'd rather leave the
 *     camera's clock untouched than overwrite it with a stale value.
 *
 * URL format follows the Lua-verified template: each of the six time fields
 * (year mod 100, month, day, hour, minute, second) is URL-encoded as %XX hex.
 *
 * TODO(§17.13): apply can_manager tz offset before encoding so the camera's
 * local-time fields are correct.  Currently sends UTC as-is.
 */
void rc_send_datetime(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    if (!ctx->last_ip) return;

    camera_model_t model = camera_manager_get_model(slot);
    if (!gopro_model_supports_http_datetime(model)) {
        ESP_LOGD(TAG, "slot %d: datetime skipped — model has no HTTP datetime path",
                 slot);
        return;
    }

    if (!can_manager_utc_is_session_synced()) {
        ESP_LOGD(TAG, "slot %d: skipping datetime — UTC not session-synced", slot);
        return;
    }

    time_t  t;
    struct tm now;
    time(&t);
    gmtime_r(&t, &now);

    char path[80];
    snprintf(path, sizeof(path), RC_HTTP_PATH_DATETIME_FMT,
             (now.tm_year + 1900) % 100,
             now.tm_mon + 1,
             now.tm_mday,
             now.tm_hour,
             now.tm_min,
             now.tm_sec);

    int code = rc_http_get(ctx->last_ip, path, RC_HTTP_TIMEOUT_MS, NULL, 0);
    if (code == 200) {
        ESP_LOGI(TAG, "slot %d: datetime set OK", slot);
    } else {
        ESP_LOGD(TAG, "slot %d: datetime set → HTTP %d", slot, code);
    }
}

/* ---- Sync time all handler (called from work task on RC_CMD_SYNC_TIME_ALL) */

void rc_handle_sync_time_all(void)
{
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        if (s_ctx[i].wifi_ready) {
            rc_send_datetime(i);
        }
    }
}

/*
 * command.c — Plain HTTP/1.0 GET helper, shutter task, and date/time sync.
 *
 * Hero4 cameras return HTTP 500 to HTTP/1.1 requests with standard headers
 * (Host, Connection, etc.).  All requests to command endpoints use HTTP/1.0
 * via raw lwip BSD sockets to avoid this.
 *
 * §17.2, §17.7, §17.8 of camera_manager_design.md.
 */

#include <stdio.h>
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
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/cmd";

/* ---- Plain HTTP/1.0 GET helper ------------------------------------------- */

/*
 * Build and send a minimal HTTP/1.0 GET request, then read the response.
 * HTTP/1.0 closes the connection after the response so we read until EOF.
 *
 * Returns the HTTP status code (e.g. 200) on success, or -1 on error.
 * If resp_buf / buf_len are provided, the response body is written there
 * (NUL-terminated; silently truncated if the body exceeds buf_len-1).
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
        ESP_LOGI(TAG, "connect() failed: errno=%d", errno);
        close(sock);
        return -1;
    }

    /* Format IP for Host header.  ip is in network byte order. */
    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));

    /* Minimal HTTP/1.0 request — no extra headers that confuse Hero4. */
    char req[512];
    int  req_len = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n",
                            path, ip_str);
    if (send(sock, req, req_len, 0) < 0) {
        ESP_LOGD(TAG, "send() failed: %d", errno);
        close(sock);
        return -1;
    }

    /* Read the full response into a single buffer (HTTP/1.0 closes on done). */
    char  raw[RC_HTTP_CMD_RESP_MAX];
    int   total = 0;
    int   n;
    int   raw_sz = (resp_buf && buf_len > RC_HTTP_CMD_RESP_MAX)
                 ? (int)buf_len  /* caller provided bigger buffer — use it */
                 : (int)sizeof(raw);
    char *rbuf   = (resp_buf && buf_len > RC_HTTP_CMD_RESP_MAX) ? resp_buf : raw;

    while ((n = recv(sock, rbuf + total, raw_sz - 1 - total, 0)) > 0) {
        total += n;
        if (total >= raw_sz - 1) break;
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

    /* Copy body (after "\r\n\r\n") to caller's buffer when using the small
     * internal raw[] and the caller supplied a separate resp_buf. */
    if (resp_buf && buf_len > 0 && rbuf == raw) {
        const char *body = strstr(raw, "\r\n\r\n");
        if (body) {
            body += 4;
            size_t body_len = (size_t)(raw + total - body);
            size_t copy     = body_len < buf_len - 1 ? body_len : buf_len - 1;
            memcpy(resp_buf, body, copy);
            resp_buf[copy] = '\0';
        } else {
            resp_buf[0] = '\0';
        }
    } else if (resp_buf && rbuf == resp_buf) {
        /* Caller's large buffer was used directly — strip headers in-place. */
        char *body = strstr(resp_buf, "\r\n\r\n");
        if (body) {
            body += 4;
            size_t body_len = (size_t)(resp_buf + total - body);
            memmove(resp_buf, body, body_len + 1);
        } else {
            resp_buf[0] = '\0';
        }
    }

    return status;
}

/* ---- Shutter task (priority 7) ------------------------------------------- */

/*
 * Loop forever on s_shutter_queue.  For each command, iterate all wifi_ready
 * slots and issue the appropriate HTTP GET.  Sequential across 4 cameras; the
 * design notes this should complete in < 200 ms total. (§17.7)
 */
void rc_shutter_task(void *arg)
{
    (void)arg;
    rc_shutter_cmd_t cmd;
    while (1) {
        xQueueReceive(s_shutter_queue, &cmd, portMAX_DELAY);
        bool        start = (cmd == RC_SHUTTER_START);
        const char *path  = start ? RC_HTTP_PATH_SHUTTER_START
                                  : RC_HTTP_PATH_SHUTTER_STOP;

        for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
            if (!s_ctx[i].wifi_ready) continue;
            int code = rc_http_get(s_ctx[i].last_ip, path,
                                    RC_HTTP_TIMEOUT_MS, NULL, 0);
            if (code == 200) {
                ESP_LOGI(TAG, "slot %d: shutter %s OK",
                         i, start ? "start" : "stop");
            } else {
                ESP_LOGW(TAG, "slot %d: shutter %s → HTTP %d",
                         i, start ? "start" : "stop", code);
            }
        }
    }
}

/* ---- Date/time sync ------------------------------------------------------ */

/*
 * Send the current local time to a single slot's camera.
 * Skipped unless UTC has been live-synced this session by either the CAN
 * 0x602 frame or a manual web-UI set.  An NVS-restored UTC at boot does NOT
 * unlock this path — we'd rather leave the camera's own clock alone than
 * overwrite it with a stale value.
 *
 * TODO: apply the per-device timezone offset from can_manager (currently
 * sends UTC).
 */
void rc_send_datetime(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    if (!ctx->last_ip) return;

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
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
             now.tm_hour, now.tm_min, now.tm_sec);

    int code = rc_http_get(ctx->last_ip, path, RC_HTTP_TIMEOUT_MS, NULL, 0);
    if (code == 200) {
        ESP_LOGI(TAG, "slot %d: datetime set OK", slot);
    } else {
        ESP_LOGW(TAG, "slot %d: datetime set → HTTP %d", slot, code);
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

/*
 * open_gopro_http_internal.h — Private shared types and declarations.
 *
 * Not part of the public include path.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "camera_manager.h"
#include "open_gopro_http_spec.h"

/* ---- Command type sent to the per-slot worker task ----------------------- */

typedef enum {
    HTTP_CMD_START_RECORDING,
    HTTP_CMD_STOP_RECORDING,
} gopro_http_cmd_t;

/* ---- Per-slot context ----------------------------------------------------- */

typedef struct {
    int      slot;
    uint32_t ip;             /* IPv4 in network byte order (from wifi_manager) */
    char     user[32];
    char     pass[64];

    /* Cached recording status — updated by worker task, read non-blocking (§8). */
    camera_recording_status_t  cached_status;
    SemaphoreHandle_t          status_mutex;

    /* Worker task: runs probe then loops polling + executing commands. */
    QueueHandle_t  cmd_queue;
    TaskHandle_t   task_handle;
    volatile bool  stop_requested;

    /* Consecutive 401 counter — triggers BLE re-provisioning at threshold. */
    uint8_t auth_fail_count;
} gopro_http_ctx_t;

/* ---- http_client.c ------------------------------------------------------- */

/*
 * Issue an HTTPS GET to the camera at ctx->ip, using Basic Auth from
 * ctx->user / ctx->pass.
 *
 * If resp_buf is non-NULL and buf_len > 0, the response body is written there
 * (NUL-terminated, truncated to buf_len-1 bytes on overflow).
 *
 * Returns the HTTP status code (200, 401, …) on success.
 * Returns a negative value on transport or TLS failure.
 */
int gopro_http_get(gopro_http_ctx_t *ctx, const char *path,
                   char *resp_buf, size_t buf_len);

/* ---- status.c ------------------------------------------------------------ */

/*
 * Blocking probe: try GET /gopro/camera/state up to GOPRO_HTTP_PROBE_RETRIES
 * times.  On success, updates cached_status and calls
 * camera_manager_on_wifi_connected().  On failure, logs and returns silently
 * (wifi_status stays WIFI_CAM_CONNECTED; next on_wifi_associated will retry).
 */
void gopro_http_probe(gopro_http_ctx_t *ctx);

/*
 * Blocking status poll: GET /gopro/camera/state, update cached_status.
 * Handles 401 by incrementing auth_fail_count and triggering re-provisioning
 * at GOPRO_HTTP_AUTH_FAIL_MAX consecutive failures.
 */
void gopro_http_poll_status(gopro_http_ctx_t *ctx);

/*
 * open_gopro_http_spec.h — Open GoPro HTTP API constants.
 *
 * All URL paths, port, timeout, and JSON key literals used by this component.
 * No other .c file embeds raw string literals for protocol values.
 *
 * Spec reference: https://gopro.github.io/OpenGoPro/http
 */
#pragma once

/* COHN HTTPS port (camera acts as HTTPS server on the home network). */
#define GOPRO_HTTP_PORT  443

/* esp_http_client socket + HTTP timeout. */
#define GOPRO_HTTP_TIMEOUT_MS  5000

/* Number of times to retry a probe before giving up. */
#define GOPRO_HTTP_PROBE_RETRIES  3u

/* Delay between probe retries. */
#define GOPRO_HTTP_PROBE_RETRY_MS  2000u

/* Recording status poll interval — matches camera_manager mismatch poll cadence. */
#define GOPRO_HTTP_POLL_INTERVAL_MS  2000u

/* Consecutive 401 responses before triggering BLE re-provisioning. */
#define GOPRO_HTTP_AUTH_FAIL_MAX  3u

/* ---- Open GoPro HTTP command paths --------------------------------------- */

/*
 * All GoPro HTTP commands are GET requests, even shutter start/stop.
 * Spec: https://gopro.github.io/OpenGoPro/http#tag/Camera-Control
 */
#define GOPRO_HTTP_PATH_SHUTTER_START  "/gopro/camera/shutter/start"
#define GOPRO_HTTP_PATH_SHUTTER_STOP   "/gopro/camera/shutter/stop"
#define GOPRO_HTTP_PATH_STATE          "/gopro/camera/state"

/*
 * Key path into /gopro/camera/state JSON response used to determine
 * the camera's current encoding (recording) state.
 *
 * Response shape: { "status": { "<id>": <int>, ... }, "settings": {...} }
 *
 * Status ID 8 = system_record_mode_active: 0 = idle, 1 = recording.
 * Spec: https://gopro.github.io/OpenGoPro/http#tag/Camera-State
 */
#define GOPRO_HTTP_STATUS_OBJ_KEY       "status"
#define GOPRO_HTTP_STATUS_ENCODING_ID   "8"

/* ---- URL and buffer sizing ----------------------------------------------- */

/* "https://NNN.NNN.NNN.NNN:443/gopro/camera/shutter/start" + NUL */
#define GOPRO_HTTP_URL_MAX     64u

/* Enough for the full /gopro/camera/state JSON body. */
#define GOPRO_HTTP_RESP_MAX  4096u

/* ---- FreeRTOS worker task parameters ------------------------------------- */

#define GOPRO_HTTP_TASK_STACK_BYTES  8192u
#define GOPRO_HTTP_TASK_PRIORITY     5u

/* Command queue depth — at most one pending start + one pending stop at once. */
#define GOPRO_HTTP_CMD_QUEUE_DEPTH  4u

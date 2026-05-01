/*
 * gopro_wifi_rc_spec.h — Protocol constants for the GoPro WiFi Remote emulation
 * driver.  All raw values (ports, payload strings, paths, field names) live here
 * so that nothing magic appears in .c files.  §17 of camera_manager_design.md.
 */
#pragma once

/* ---- UDP (§17.2) --------------------------------------------------------- */

/* Keepalive payload sent unicast to each camera at TX_PORT every 3 s. */
#define RC_UDP_KEEPALIVE_PAYLOAD    "_GPHD_:0:0:2:0.000000\n"
/* Camera -> ESP32 ACK arrives on RX_PORT; first byte == ACK_BYTE. */
#define RC_UDP_TX_PORT              8484
#define RC_UDP_RX_PORT              8383
#define RC_UDP_WOL_PORT             9
#define RC_UDP_KEEPALIVE_ACK_BYTE   0x5F

/* ---- HTTP (§17.2) -------------------------------------------------------- */

/* Plain HTTP, port 80.  HTTP/1.0 required — Hero4 returns 500 on HTTP/1.1. */
#define RC_HTTP_PORT                80

/* GoPro old WiFi API endpoints */
#define RC_HTTP_PATH_STATUS         "/gp/gpControl/status"
#define RC_HTTP_PATH_SHUTTER_START  "/gp/gpControl/command/shutter?p=1"
#define RC_HTTP_PATH_SHUTTER_STOP   "/gp/gpControl/command/shutter?p=0"
/* printf format: year, month, day, hour, min, sec (all local time) */
#define RC_HTTP_PATH_DATETIME_FMT   "/gp/gpControl/command/setup/date_time?p=%04d_%02d_%02d_%02d_%02d_%02d"

/* ---- JSON field IDs (old GoPro WiFi API status object) ------------------- */

#define RC_STATUS_JSON_OBJ          "status"
/* Encoding / recording state: 0 = idle, non-zero = recording. */
#define RC_STATUS_JSON_ENCODING     "8"
/* Camera name string.
 * TODO(hardware): verify field "30" is camera name on Hero4.  The field may
 * need to be read from /gp/gpControl/info instead. */
#define RC_STATUS_JSON_NAME         "30"

/* ---- Timing -------------------------------------------------------------- */

#define RC_KEEPALIVE_INTERVAL_MS    3000   /* Per-slot UDP keepalive period */
#define RC_WOL_RETRY_INTERVAL_MS    2000   /* Per-slot WoL retry period */
#define RC_STATUS_POLL_INTERVAL_MS  5000   /* Global HTTP status poll period */
/* WoL retry fires when no ACK received for this long after keepalive armed. */
#define RC_KEEPALIVE_SILENCE_MS     10000

/* ---- HTTP timeouts ------------------------------------------------------- */

#define RC_HTTP_TIMEOUT_MS          2000
#define RC_PROBE_TIMEOUT_MS         5000
#define RC_PROBE_RETRIES            3
#define RC_PROBE_RETRY_MS           2000

/* ---- Wake-on-LAN --------------------------------------------------------- */

#define RC_WOL_BURST                5      /* Magic packets sent per WoL event */

/* ---- Task / queue parameters --------------------------------------------- */

#define RC_WORK_TASK_PRIORITY       5
#define RC_WORK_TASK_STACK_BYTES    4096
#define RC_SHUTTER_TASK_PRIORITY    7
#define RC_SHUTTER_TASK_STACK_BYTES 4096
#define RC_UDP_RX_TASK_PRIORITY     4
#define RC_UDP_RX_TASK_STACK_BYTES  2048

#define RC_WORK_QUEUE_DEPTH         16
#define RC_SHUTTER_QUEUE_DEPTH      8

/* ---- Response buffer sizes ----------------------------------------------- */

/* Status endpoint response can be several KB on Hero4. */
#define RC_HTTP_STATUS_RESP_MAX     4096
/* Command endpoint responses are tiny; we only care about the status code. */
#define RC_HTTP_CMD_RESP_MAX        256

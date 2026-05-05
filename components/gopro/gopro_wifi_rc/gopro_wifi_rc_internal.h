/*
 * gopro_wifi_rc_internal.h — Private types, globals, and forward declarations
 * shared across the gopro_wifi_rc source files.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "camera_manager.h"
#include "gopro_wifi_rc_spec.h"

/* ---- Per-slot driver context (§17.3) ------------------------------------- */

typedef struct {
    int                        slot;
    uint8_t                    mac[6];           /* for WoL target MAC */
    uint32_t                   last_ip;          /* network byte order */
    camera_recording_status_t  recording_status; /* cache updated by RX task */
    bool                       wifi_ready;
    bool                       identify_attempted; /* HTTP /gp/gpControl probe ran this session */
    /* Updated by the UDP RX task on every datagram from the slot's IP
     * (keepalive ACK or `st` response).  Compared by keepalive_tick to drive
     * the WoL retry watchdog. */
    TickType_t                 last_response_tick;
    esp_timer_handle_t         keepalive_timer;  /* 3 s periodic, armed per slot */
    esp_timer_handle_t         wol_retry_timer;  /* 2 s periodic, armed on silence */
} gopro_wifi_rc_ctx_t;

/* ---- Work queue message (§17.4) ------------------------------------------ */

typedef enum {
    RC_CMD_STATION_ASSOCIATED,
    RC_CMD_STATION_DHCP,
    RC_CMD_STATION_DISCONNECTED,
    RC_CMD_KEEPALIVE_TICK,
    RC_CMD_WOL_RETRY,
    RC_CMD_STATUS_POLL_ALL,
    RC_CMD_PROMOTE,                    /* RX task → flip wifi_ready, fire identify + datetime */
    RC_CMD_HTTP_IDENTIFY,              /* one-shot GET /gp/gpControl */
    RC_CMD_SYNC_TIME_ALL,
    /* RC_CMD_PROBE is removed — the old HTTP probe loop is gone; readiness is
     * driven by the first UDP response (see rc_handle_promote). */
} rc_work_cmd_type_t;

typedef struct {
    rc_work_cmd_type_t type;
    union {
        struct { uint8_t mac[6]; }              mac_only; /* ASSOCIATED, DISCONNECTED */
        struct { uint8_t mac[6]; uint32_t ip; } mac_ip;   /* DHCP */
        struct { int slot; }                    slot_cmd; /* KEEPALIVE_TICK, WOL_RETRY,
                                                             PROMOTE, HTTP_IDENTIFY */
    };
} rc_work_cmd_t;

typedef enum { RC_SHUTTER_START, RC_SHUTTER_STOP } rc_shutter_cmd_t;

/* ---- Globals (defined in driver.c) --------------------------------------- */

extern gopro_wifi_rc_ctx_t s_ctx[CAMERA_MAX_SLOTS];
extern QueueHandle_t       s_work_queue;
extern QueueHandle_t       s_shutter_queue;
extern int                 s_udp_sock; /* shared UDP socket bound to RC_UDP_RX_PORT;
                                          -1 if not open */

/* ---- connection.c -------------------------------------------------------- */

void rc_handle_station_associated(const uint8_t mac[6]);
void rc_handle_station_dhcp(const uint8_t mac[6], uint32_t ip);
void rc_handle_station_disconnected(const uint8_t mac[6]);
void rc_handle_promote(int slot);            /* replaces rc_handle_probe */
void rc_handle_http_identify(int slot);      /* GET /gp/gpControl, parse JSON, set model */
void rc_handle_keepalive_tick(int slot);
void rc_handle_wol_retry(int slot);
void rc_arm_keepalive_timer(gopro_wifi_rc_ctx_t *ctx);
void rc_disarm_keepalive_timer(gopro_wifi_rc_ctx_t *ctx);
void rc_arm_wol_retry_timer(gopro_wifi_rc_ctx_t *ctx);
void rc_disarm_wol_retry_timer(gopro_wifi_rc_ctx_t *ctx);

/* ---- command.c ----------------------------------------------------------- */

void rc_shutter_task(void *arg);
void rc_handle_sync_time_all(void);
void rc_send_datetime(int slot);

/*
 * Issue a plain HTTP/1.0 GET to camera at ip:RC_HTTP_PORT.
 * timeout_ms applies independently to send and recv on the TCP socket.
 * Returns HTTP status code (200 / 4xx / 5xx) or -1 on transport failure.
 * If resp_buf is non-NULL and buf_len > 0, the response body is written there
 * (NUL-terminated, silently truncated on overflow).
 */
int rc_http_get(uint32_t ip, const char *path, int timeout_ms,
                char *resp_buf, size_t buf_len);

/*
 * Substring-extract three fields from the JSON body of GET /gp/gpControl:
 *   info.model_name      → name_out (NUL-terminated)
 *   info.model_number    → *model_number_out (or -1 if absent)
 *   info.firmware_version → fw_out (NUL-terminated)
 *
 * Returns true if model_name was found.  Empty/absent fields leave their
 * out-params as empty strings / -1.  No cJSON dependency — uses strstr-based
 * parsing tuned for the known GoPro JSON layout.
 */
bool rc_parse_identify_json(const char *body,
                            char *name_out, size_t name_size,
                            int  *model_number_out,
                            char *fw_out,   size_t fw_size);

/* ---- status.c ------------------------------------------------------------ */

void rc_handle_status_poll_all(void);

/*
 * Decode a 20-byte `st` response: bytes 13/14/15 → recording_status.
 * Called by the UDP RX task when an opcode-`st` datagram arrives from a
 * known slot; updates ctx->recording_status in place.
 */
void rc_parse_st_response(int slot, const uint8_t *buf, int len);

/* ---- udp.c --------------------------------------------------------------- */

esp_err_t rc_udp_init(void);
void      rc_send_keepalive(uint32_t ip);
void      rc_send_st(uint32_t ip);                       /* status request */
void      rc_send_sh(uint32_t ip, bool start);           /* shutter start/stop */
void      rc_send_wol(uint32_t ip, const uint8_t mac[6]);
void      rc_udp_rx_task(void *arg);

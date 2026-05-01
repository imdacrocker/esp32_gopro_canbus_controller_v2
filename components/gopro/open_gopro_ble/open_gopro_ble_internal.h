/*
 * open_gopro_ble_internal.h — Private shared types and declarations.
 *
 * Not part of the public include path.  Only included by the .c files inside
 * this component.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "open_gopro_ble.h"
#include "open_gopro_ble_spec.h"
#include "camera_manager.h"
#include "ble_core.h"

/* ---- GATT handle table (one per connected slot) -------------------------- */

typedef struct {
    uint16_t cmd_write;
    uint16_t cmd_resp_notify;
    uint16_t settings_write;
    uint16_t settings_resp_notify;
    uint16_t query_write;
    uint16_t query_resp_notify;
    uint16_t net_mgmt_cmd_write;
    uint16_t net_mgmt_resp_notify;
    uint16_t wifi_ap_pwr_write;
    uint16_t wifi_ap_ssid_read;
    uint16_t wifi_ap_pass_read;
    uint16_t wifi_ap_state_indicate;
} gopro_gatt_handles_t;

/* ---- Per-slot driver context (§15.4) ------------------------------------- */

typedef struct {
    uint16_t             conn_handle;
    int                  slot;
    gopro_gatt_handles_t gatt;
    uint16_t             negotiated_mtu;

    /* Readiness poll state */
    bool               readiness_polling;
    uint8_t            readiness_retry_count;
    esp_timer_handle_t readiness_timer;

    /* BLE keepalive */
    esp_timer_handle_t keepalive_timer;

    /* COHN provisioning state */
    bool cohn_provisioning;
    bool cohn_pending_utc;  /* deferred until sync_time_all() fires */
} gopro_ble_ctx_t;

/* Sentinel for "no connection" */
#define GOPRO_CONN_NONE  BLE_HS_CONN_HANDLE_NONE

/* ---- Context management (driver.c) --------------------------------------- */

/* Returns ctx for slot, or NULL if slot is out of range / uninitialized. */
gopro_ble_ctx_t *gopro_ctx_by_slot(int slot);

/* Returns ctx whose conn_handle matches, or NULL. */
gopro_ble_ctx_t *gopro_ctx_by_conn(uint16_t conn_handle);

/* ---- GATT discovery (gatt.c) --------------------------------------------- */

/* Start MTU exchange + characteristic discovery.  Called from pairing.c. */
void gopro_gatt_start_discovery(gopro_ble_ctx_t *ctx);

/* ---- Readiness poll (readiness.c) ---------------------------------------- */

/* Begin sending GetHardwareInfo; arms readiness_timer. */
void gopro_readiness_start(gopro_ble_ctx_t *ctx);

/* Stop and delete the readiness timer.  Safe if timer was never started. */
void gopro_readiness_cancel(gopro_ble_ctx_t *ctx);

/*
 * Called by readiness.c after the camera confirms it is ready.
 * Checks NVS for COHN credentials and branches to provisioning or ready state.
 */
void gopro_on_camera_ready(gopro_ble_ctx_t *ctx, uint32_t model_num);

/* ---- Control (control.c) ------------------------------------------------- */

/* Send SetDateTime to a connected slot.  Best-effort, no retry on failure. */
void gopro_control_set_datetime(gopro_ble_ctx_t *ctx);

/* Start the 3-second periodic BLE keepalive timer. */
void gopro_keepalive_start(gopro_ble_ctx_t *ctx);

/* Stop and delete the keepalive timer.  Safe if never started. */
void gopro_keepalive_stop(gopro_ble_ctx_t *ctx);

/* ---- COHN provisioning (cohn.c) ----------------------------------------- */

/* Run the full COHN provisioning sequence on an established BLE connection. */
void gopro_cohn_provision(gopro_ble_ctx_t *ctx);

/* ---- Response reassembly (query.c) --------------------------------------- */

typedef enum {
    GOPRO_CHAN_CMD,       /* cmd_resp_notify  (GP-0073) */
    GOPRO_CHAN_SETTINGS,  /* settings_resp_notify (GP-0075) */
    GOPRO_CHAN_QUERY,     /* query_resp_notify (GP-0077) */
    GOPRO_CHAN_NET_MGMT,  /* net_mgmt_resp_notify (GP-0092) */
} gopro_channel_t;

/*
 * Feed an incoming ATT notification into the per-slot reassembler for the
 * given channel.  Calls the appropriate handler when a complete response
 * has been accumulated.
 */
void gopro_query_feed(gopro_ble_ctx_t *ctx, gopro_channel_t chan,
                      const uint8_t *data, uint16_t len);

/* Free reassembly state for a slot (called on disconnect). */
void gopro_query_free(gopro_ble_ctx_t *ctx);

/* ---- Notification routing (notify.c) ------------------------------------- */

/* Called by ble_core's on_notify_rx callback. */
void gopro_notify_rx(uint16_t conn_handle, uint16_t attr_handle,
                     const uint8_t *data, uint16_t len);

/* ---- GPBS packet helpers ------------------------------------------------- */

/*
 * Encode a GPBS general header for payload_len bytes.
 * payload_len must be <= GPBS_HDR_GENERAL_MAX (31).
 * Returns header byte count (1).
 */
static inline int gpbs_write_hdr(uint8_t *buf, uint8_t payload_len)
{
    buf[0] = payload_len & GPBS_HDR_GENERAL_MAX;
    return 1;
}

/*
 * Encode a GPBS extended-13 header for payload_len bytes (32–8191).
 * Returns header byte count (2).
 */
static inline int gpbs_write_hdr13(uint8_t *buf, uint16_t payload_len)
{
    buf[0] = GPBS_HDR_EXT13 | ((payload_len >> 8) & 0x1Fu);
    buf[1] = payload_len & 0xFFu;
    return 2;
}

/*
 * open_gopro_ble.h — Public API for the GoPro BLE provisioning component.
 *
 * This component handles discovery, pairing, and COHN provisioning over BLE.
 * It is the only component that directly uses the ble_core callbacks.
 * Recording commands travel over HTTPS via open_gopro_http (not BLE).
 *
 * §15 of camera_manager_design.md
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "host/ble_hs.h"
#include "camera_manager.h"

/* Maximum number of cameras shown in the discovery list. */
#define GOPRO_DISC_MAX  10

/* ---- Discovery ----------------------------------------------------------- */

typedef struct {
    ble_addr_t addr;
    int8_t     rssi;
    char       name[32];
} gopro_device_t;

/*
 * Start a user-initiated BLE scan for GoPro cameras.
 * Advertisements matching the GoPro service UUID (0xFEA6) are added to the
 * internal discovery list.  Wraps ble_core_start_discovery().
 */
void open_gopro_ble_start_discovery(void);

/* Cancel the discovery scan. */
void open_gopro_ble_stop_discovery(void);

/*
 * Copy up to max_count discovered devices into out[].
 * Returns the number of entries written.
 * Safe to call from any task.
 */
int open_gopro_ble_get_discovered(gopro_device_t *out, int max_count);

/* ---- Connection ---------------------------------------------------------- */

/*
 * Initiate a connection to a camera by BLE address.
 * Wraps ble_core_connect_by_addr().
 */
void open_gopro_ble_connect_by_addr(const ble_addr_t *addr);

/* ---- COHN credentials ---------------------------------------------------- */

/*
 * Read the COHN credentials for slot from NVS.
 * Returns true if credentials exist and were copied into the output buffers.
 * Called by open_gopro_http to construct Basic Auth headers.
 */
bool open_gopro_ble_get_cohn_credentials(int slot,
                                          char *user_out, size_t user_len,
                                          char *pass_out, size_t pass_len);

/* ---- Re-provisioning ----------------------------------------------------- */

/*
 * Clear the NVS COHN credentials for slot and re-run the provisioning
 * sequence on the existing BLE connection.
 * Called by open_gopro_http after N consecutive HTTPS 401 responses.
 */
void open_gopro_ble_reprovision(int slot);

/* ---- UTC sync ------------------------------------------------------------ */

/*
 * Send SetDateTime to every currently-connected camera and unblock any
 * provisioning sequences that were waiting for a valid UTC timestamp.
 * Called by can_manager when a GPS fix is acquired, or by the web UI
 * on a manual time-set request.
 */
void open_gopro_ble_sync_time_all(void);

/* ---- Component lifecycle ------------------------------------------------- */

/*
 * Register BLE callbacks with ble_core, register the driver with
 * camera_manager, and purge stale bonds.
 *
 * Must be called after camera_manager_init() and before ble_core_init().
 */
void open_gopro_ble_init(void);

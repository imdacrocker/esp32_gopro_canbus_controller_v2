/*
 * gopro_wifi_rc.h — Public API for the GoPro WiFi Remote emulation driver.
 *
 * Implements camera_driver_t for Hero4 Black and Hero4 Silver cameras.
 * These cameras connect to the SoftAP automatically when they see the correct
 * SSID (HERO-RC-XXXXXX) and OUI (d8:96:85) — both configured by wifi_manager.
 * Recording commands travel over UDP (keepalive) and plain HTTP/1.0 (shutter,
 * status, date/time).  No BLE is used.
 *
 * §17 of camera_manager_design.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Register the RC-emulation driver with camera_manager, start the work and
 * shutter tasks, open the UDP socket, and start the global status-poll timer.
 *
 * Must be called after camera_manager_init() and before wifi_manager_init().
 */
void gopro_wifi_rc_init(void);

/* ---- Station lifecycle callbacks (wired in main.c §21.3) ---------------- */

/* Called by main on_station_associated.  Must not block. */
void gopro_wifi_rc_on_station_associated(const uint8_t mac[6]);

/* Called by main on_station_dhcp.  Must not block. */
void gopro_wifi_rc_on_station_dhcp(const uint8_t mac[6], uint32_t ip);

/* Called by main on_station_disassociated.  Must not block. */
void gopro_wifi_rc_on_station_disassociated(const uint8_t mac[6]);

/* ---- Manual add from web UI (POST /api/rc/add) --------------------------- */

/*
 * Register mac as a new RC-emulation camera, seed it with ip, and trigger an
 * HTTP probe.  Called by http_server; mac must already be associated to the
 * SoftAP with a valid DHCP lease.
 *
 * Defaults model to CAMERA_MODEL_GOPRO_HERO4_BLACK — model picker in the web
 * UI is a future TODO (web_ui_spec.md §17).
 */
void gopro_wifi_rc_add_camera(const uint8_t mac[6], uint32_t ip);

/*
 * Tear down resources for slot.  Called from http_server before it invokes
 * camera_manager_remove_slot(); driver teardown is also called by
 * camera_manager_remove_slot() itself, so this is only needed for cleanup
 * that must happen before the slot record is erased.
 */
void gopro_wifi_rc_remove_camera(int slot);

/* ---- Predicates used by http_server for /api/rc/discovered --------------- */

/* True if slot is managed by this driver (uses RC-emulation model). */
bool gopro_wifi_rc_is_managed_slot(int slot);

/* True if mac belongs to a configured RC-emulation camera slot. */
bool gopro_wifi_rc_is_managed_mac(const uint8_t mac[6]);

/* ---- UTC sync (called from main.c on_utc_acquired) ----------------------- */

/*
 * Send the current date/time to all wifi-ready RC-emulation cameras.
 * Reads system time via time(); requires the system clock to be set by
 * can_manager before this fires (first valid GPS frame from RaceCapture).
 */
void gopro_wifi_rc_sync_time_all(void);

/* ---- DIAGNOSTIC (temporary) ---------------------------------------------- *
 *
 * Spawn a one-shot task that runs a battery of network probes against the
 * camera at (mac, ip) and logs the results to the serial console:
 *   - ICMP ping (5 packets)
 *   - TCP port sweep across common GoPro / general ports
 *   - HTTP/1.1 GET probes on any responding TCP ports against several known
 *     GoPro endpoint paths
 *   - Extra UDP opcode probes (`cv` GET-form 0/1, `wt`); replies are logged
 *     by the existing rc_udp_rx_task
 *
 * While this is wired in, the /api/rc/add web endpoint dispatches here
 * INSTEAD of the normal pair flow.  Revert by swapping the call back to
 * gopro_wifi_rc_add_camera() in components/http_server/api_rc.c.
 */
void gopro_wifi_rc_diagnose(const uint8_t mac[6], uint32_t ip);

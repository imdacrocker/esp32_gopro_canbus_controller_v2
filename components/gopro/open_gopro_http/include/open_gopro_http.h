/*
 * open_gopro_http.h — Public API for the GoPro COHN HTTPS driver component.
 *
 * This component registers a camera_driver_t with camera_manager for all
 * COHN-capable GoPro models (Hero 9 and later).  It handles:
 *   - Probing the camera over HTTPS after COHN provisioning completes
 *   - Issuing start/stop shutter commands
 *   - Polling the camera's encoding state for mismatch correction
 *   - Triggering re-provisioning via open_gopro_ble after repeated 401s
 *
 * HTTPS uses Basic Auth with credentials stored by open_gopro_ble/cohn.c.
 * The camera's self-signed TLS certificate is not verified (private LAN).
 * Requires CONFIG_ESP_TLS_INSECURE=y and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
 * in sdkconfig.
 */
#pragma once

#include <stdint.h>

/*
 * Register the COHN HTTPS driver with camera_manager.
 * Must be called after camera_manager_init() and before wifi_manager_init().
 */
void open_gopro_http_init(void);

/*
 * Notify the driver that a SoftAP station with the given MAC has disconnected.
 * Called from main's on_station_disassociated callback for all MACs; the driver
 * applies its own model-type guard and ignores non-COHN slots.
 * Must not block — called on the wifi_manager event task.
 */
void open_gopro_http_on_camera_disconnected_by_mac(const uint8_t mac[6]);

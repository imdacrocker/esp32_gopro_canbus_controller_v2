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

/*
 * Register the COHN HTTPS driver with camera_manager.
 * Must be called after camera_manager_init() and before wifi_manager_init().
 */
void open_gopro_http_init(void);


#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "ble_core.h"
#include "camera_manager.h"
#include "open_gopro_ble.h"
#include "open_gopro_http.h"
#include "gopro_wifi_rc.h"
#include "can_manager.h"
#include "http_server.h"

static const char *TAG = "main";

/* ---- CAN callbacks ------------------------------------------------------- */

static void on_gps_utc_acquired(uint64_t utc_ms, void *arg)
{
    (void)utc_ms; (void)arg;
    open_gopro_ble_sync_time_all();
    gopro_wifi_rc_sync_time_all();
}

/* ---- WiFi station callbacks (§21.3) -------------------------------------- */

static void on_station_associated(const uint8_t mac[6])
{
    gopro_wifi_rc_on_station_associated(mac);
    /* COHN cameras: open_gopro_ble tracks the SoftAP join internally via
     * RequestGetCOHNStatus polling — no callback needed here. */
}

static void on_station_disconnected(const uint8_t mac[6])
{
    gopro_wifi_rc_on_station_disassociated(mac);           /* RC-emulation path */
    open_gopro_http_on_camera_disconnected_by_mac(mac);    /* COHN path         */
    /* Each handler applies its own model-type guard — only the owning driver acts. */
}

static void on_station_ip_assigned(const uint8_t mac[6], uint32_t ip)
{
    camera_manager_on_station_ip(mac, ip);   /* updates last_ip for any matching slot */
    gopro_wifi_rc_on_station_dhcp(mac, ip);
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot: NimBLE core=%d, WiFi core=%d, channel=%d",
             CONFIG_BT_NIMBLE_PINNED_TO_CORE,
             CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0 ? 0 : 1,
             AP_CHANNEL);

    /* NVS is required by BLE for bonding info. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    camera_manager_init();

    open_gopro_http_init();

    /* Registers RC-emulation driver, starts work/shutter/UDP tasks. */
    gopro_wifi_rc_init();

    /* Registers BLE callbacks with ble_core and purges stale bonds.
     * Must be called before ble_core_init(). */
    open_gopro_ble_init();

    /* Starts the NimBLE host task. on_sync fires async and begins scanning. */
    ble_core_init();

    /* Wire CAN callbacks before starting the TWAI driver. */
    can_manager_callbacks_t can_cbs = {
        .on_logging_state     = NULL,   /* camera_manager handles intent directly */
        .on_logging_state_arg = NULL,
        .on_utc_acquired      = on_gps_utc_acquired,
        .on_utc_acquired_arg  = NULL,
        .on_rx_frame          = NULL,
        .on_rx_frame_arg      = NULL,
    };
    can_manager_register_callbacks(&can_cbs);
    can_manager_init();

    /* Wire WiFi station events to both RC-emulation and COHN drivers (§21.3).
     * Must be called before wifi_manager_init() so no events are lost. */
    wifi_manager_set_callbacks(on_station_associated,
                               on_station_disconnected,
                               on_station_ip_assigned);

    /* Raises the SoftAP — must come after all station callbacks are wired. */
    wifi_manager_init();
    wifi_manager_wait_for_ap_ready();

    /* Mount LittleFS, start esp_httpd, register all /api/ handlers. */
    http_server_init();
}
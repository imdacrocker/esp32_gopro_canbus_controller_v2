
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "ble_core.h"
#include "camera_manager.h"

static const char *TAG = "main";

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

    /* TODO: open_gopro_http_init()   — registers COHN HTTPS driver        */
    /* TODO: gopro_wifi_rc_init()     — registers RC-emulation driver      */
    /* TODO: open_gopro_ble_init()    — registers BLE callbacks + purges bonds */

    /* Starts the NimBLE host task. on_sync fires async and begins scanning. */
    ble_core_init();

    /* TODO: can_manager_init()       — starts TWAI driver and RX task     */

    /* Raises the SoftAP — must come after all station callbacks are wired. */
    wifi_manager_init();
    wifi_manager_wait_for_ap_ready();
}
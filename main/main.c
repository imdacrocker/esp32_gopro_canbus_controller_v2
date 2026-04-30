
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi_manager.h"

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

    wifi_manager_init();
    wifi_manager_wait_for_ap_ready();
}
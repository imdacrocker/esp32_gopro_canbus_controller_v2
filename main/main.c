
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "main";

/* Channel 11 (2462 MHz) — clear of BLE advertising channels 37/38/39. See §4.2.
   Move to wifi_manager.h once that component exists. */
#define AP_CHANNEL 11

void app_main(void)
{
    ESP_LOGI(TAG, "boot: NimBLE core=%d, WiFi core=%d, channel=%d",
         CONFIG_BT_NIMBLE_PINNED_TO_CORE,
         CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0 ? 0 : 1,
         AP_CHANNEL);
         
    /* NVS is required by the BLE stack for storing bonding info. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}
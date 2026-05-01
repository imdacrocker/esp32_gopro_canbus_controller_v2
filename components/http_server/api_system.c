/*
 * api_system.c — System-level API handlers (§20.4).
 *
 * Endpoints:
 *   GET  /api/logging-state
 *   GET  /api/utc
 *   GET  /api/auto-control
 *   POST /api/auto-control
 *   POST /api/reboot
 *   POST /api/factory-reset
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "can_manager.h"
#include "camera_manager.h"
#include "http_server_internal.h"

static const char *TAG = "http_api_system";

/* ---- GET /api/logging-state ---------------------------------------------- */

static esp_err_t handler_logging_state(httpd_req_t *req)
{
    static const char * const state_str[] = {
        [LOGGING_STATE_UNKNOWN]     = "unknown",
        [LOGGING_STATE_NOT_LOGGING] = "not_logging",
        [LOGGING_STATE_LOGGING]     = "logging",
    };
    can_logging_state_t state = can_manager_get_logging_state();
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}", state_str[state]);
    send_json(req, buf);
    return ESP_OK;
}

/* ---- GET /api/utc -------------------------------------------------------- */

static esp_err_t handler_utc(httpd_req_t *req)
{
    uint64_t utc_ms = 0;
    bool valid = can_manager_get_utc_ms(&utc_ms);
    char buf[64];
    if (valid) {
        /* Apply timezone offset (§14.3). */
        int8_t tz = can_manager_get_tz_offset();
        utc_ms += (int64_t)tz * 3600LL * 1000LL;
        snprintf(buf, sizeof(buf),
                 "{\"valid\":true,\"epoch_ms\":%llu}", (unsigned long long)utc_ms);
    } else {
        snprintf(buf, sizeof(buf), "{\"valid\":false,\"epoch_ms\":0}");
    }
    send_json(req, buf);
    return ESP_OK;
}

/* ---- GET /api/auto-control ----------------------------------------------- */

static esp_err_t handler_get_auto_control(httpd_req_t *req)
{
    bool enabled = camera_manager_get_auto_control();
    send_json(req, enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    return ESP_OK;
}

/* ---- POST /api/auto-control ---------------------------------------------- */

static esp_err_t handler_post_auto_control(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *item = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'enabled'");
        return ESP_FAIL;
    }
    bool enabled = cJSON_IsTrue(item);
    cJSON_Delete(root);

    camera_manager_set_auto_control(enabled);
    ESP_LOGI(TAG, "auto-control set to %s", enabled ? "on" : "off");

    send_json(req, enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    return ESP_OK;
}

/* ---- POST /api/reboot ---------------------------------------------------- */

static esp_err_t handler_reboot(httpd_req_t *req)
{
    send_json(req, "{}");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

/* ---- POST /api/factory-reset --------------------------------------------- */

static esp_err_t handler_factory_reset(httpd_req_t *req)
{
    ESP_LOGW(TAG, "factory reset — erasing NVS");
    send_json(req, "{}");
    vTaskDelay(pdMS_TO_TICKS(100));
    nvs_flash_erase();
    esp_restart();
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_system_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/logging-state", .method = HTTP_GET,  .handler = handler_logging_state    },
        { .uri = "/api/utc",           .method = HTTP_GET,  .handler = handler_utc              },
        { .uri = "/api/auto-control",  .method = HTTP_GET,  .handler = handler_get_auto_control },
        { .uri = "/api/auto-control",  .method = HTTP_POST, .handler = handler_post_auto_control},
        { .uri = "/api/reboot",        .method = HTTP_POST, .handler = handler_reboot           },
        { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = handler_factory_reset    },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

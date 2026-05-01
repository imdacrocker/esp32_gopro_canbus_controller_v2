/*
 * api_settings.c — Device settings API handlers (§20.4).
 *
 * Endpoints:
 *   GET  /api/settings/timezone
 *   POST /api/settings/timezone
 *   POST /api/settings/datetime
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "can_manager.h"
#include "http_server_internal.h"

static const char *TAG = "http_api_settings";

/* ---- GET /api/settings/timezone ------------------------------------------ */

static esp_err_t handler_get_timezone(httpd_req_t *req)
{
    int8_t tz = can_manager_get_tz_offset();
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"tz_offset_hours\":%d}", (int)tz);
    send_json(req, buf);
    return ESP_OK;
}

/* ---- POST /api/settings/timezone ----------------------------------------- */

static esp_err_t handler_post_timezone(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *tz_item = cJSON_GetObjectItem(root, "tz_offset_hours");
    if (!cJSON_IsNumber(tz_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing tz_offset_hours");
        return ESP_FAIL;
    }
    int tz = (int)cJSON_GetNumberValue(tz_item);
    cJSON_Delete(root);

    if (tz < -12 || tz > 14) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tz_offset_hours out of range");
        return ESP_FAIL;
    }

    can_manager_set_tz_offset((int8_t)tz);
    ESP_LOGI(TAG, "timezone set to UTC%+d", tz);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- POST /api/settings/datetime ----------------------------------------- */
/*
 * Only valid when GPS time is not yet acquired.  Sets the system clock from
 * the browser and fires the UTC-acquired path (time sync to all cameras).
 */
static esp_err_t handler_post_datetime(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *ms_item = cJSON_GetObjectItem(root, "epoch_ms");
    if (!cJSON_IsNumber(ms_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing epoch_ms");
        return ESP_FAIL;
    }
    uint64_t utc_ms = (uint64_t)cJSON_GetNumberValue(ms_item);
    cJSON_Delete(root);

    esp_err_t err = can_manager_set_manual_utc_ms(utc_ms);
    if (err == ESP_ERR_INVALID_STATE) {
        /* GPS time already valid — manual override not permitted. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "GPS time already acquired");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid timestamp");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "manual datetime set: %llu ms", (unsigned long long)utc_ms);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_settings_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/settings/timezone", .method = HTTP_GET,  .handler = handler_get_timezone  },
        { .uri = "/api/settings/timezone", .method = HTTP_POST, .handler = handler_post_timezone },
        { .uri = "/api/settings/datetime", .method = HTTP_POST, .handler = handler_post_datetime },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

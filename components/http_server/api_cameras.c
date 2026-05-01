/*
 * api_cameras.c — Camera management API handlers (§20.4).
 *
 * Endpoints:
 *   GET  /api/paired-cameras
 *   POST /api/shutter
 *   POST /api/remove-camera
 *   POST /api/reorder-cameras
 *   GET  /api/cameras
 *   POST /api/scan
 *   POST /api/scan-cancel
 *   POST /api/pair
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "camera_manager.h"
#include "camera_types.h"
#include "open_gopro_ble.h"
#include "gopro_model.h"
#include "http_server_internal.h"

static const char *TAG = "http_api_cameras";

/* ---- Helpers ------------------------------------------------------------- */

static const char *model_name_str(camera_model_t model)
{
    switch (model) {
    case CAMERA_MODEL_GOPRO_HERO4_BLACK:  return "GoPro Hero4 Black";
    case CAMERA_MODEL_GOPRO_HERO4_SILVER: return "GoPro Hero4 Silver";
    case CAMERA_MODEL_GOPRO_HERO9_BLACK:  return "GoPro Hero9 Black";
    case CAMERA_MODEL_GOPRO_HERO10_BLACK: return "GoPro Hero10 Black";
    case CAMERA_MODEL_GOPRO_HERO11_BLACK: return "GoPro Hero11 Black";
    case CAMERA_MODEL_GOPRO_HERO11_MINI:  return "GoPro Hero11 Mini";
    case CAMERA_MODEL_GOPRO_HERO12_BLACK: return "GoPro Hero12 Black";
    case CAMERA_MODEL_GOPRO_MAX2:         return "GoPro MAX2";
    case CAMERA_MODEL_GOPRO_HERO13_BLACK: return "GoPro Hero13 Black";
    case CAMERA_MODEL_GOPRO_LIT_HERO:     return "GoPro Lite Hero";
    default:                              return "Unknown";
    }
}

static const char *camera_status_str(const camera_slot_info_t *info)
{
    if (info->wifi_status == WIFI_CAM_READY) {
        return info->is_recording ? "recording" : "not_recording";
    }
    if (info->ble_status >= CAM_BLE_CONNECTED ||
        info->wifi_status >= WIFI_CAM_CONNECTED) {
        return "connected";
    }
    return "disconnected";
}

/* ---- GET /api/paired-cameras --------------------------------------------- */

static esp_err_t handler_paired_cameras(httpd_req_t *req)
{
    int count = camera_manager_get_slot_count();

    /* Each slot JSON entry is at most ~200 bytes; 4 slots + brackets = ~900. */
    const size_t BUF_SIZE = 1024;
    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t pos = 0;
    buf[pos++] = '[';

    for (int i = 0; i < count; i++) {
        camera_slot_info_t info;
        if (camera_manager_get_slot_info(i, &info) != ESP_OK) continue;
        if (!info.is_configured) continue;

        char mac_str[18];
        format_mac(mac_str, info.mac);

        const char *type   = gopro_model_uses_rc_emulation(info.model)
                             ? "rc_emulation" : "ble";
        const char *status = camera_status_str(&info);

        int n = snprintf(buf + pos, BUF_SIZE - pos,
            "%s{"
            "\"slot\":%d,"
            "\"index\":%d,"
            "\"name\":\"%s\","
            "\"model_name\":\"%s\","
            "\"type\":\"%s\","
            "\"addr\":\"%s\","
            "\"status\":\"%s\""
            "}",
            (i == 0) ? "" : ",",
            i, i,
            info.name,
            model_name_str(info.model),
            type,
            mac_str,
            status);

        if (n < 0 || (size_t)n >= BUF_SIZE - pos) {
            /* Truncation — stop here rather than emit corrupt JSON. */
            break;
        }
        pos += (size_t)n;
    }

    if (pos < BUF_SIZE - 1) buf[pos++] = ']';
    buf[pos] = '\0';

    send_json(req, buf);
    free(buf);
    return ESP_OK;
}

/* ---- POST /api/shutter --------------------------------------------------- */

static esp_err_t handler_shutter(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *on_item   = cJSON_GetObjectItem(root, "on");
    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");

    if (!cJSON_IsBool(on_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'on'");
        return ESP_FAIL;
    }

    bool record = cJSON_IsTrue(on_item);
    desired_recording_t intent = record ? DESIRED_RECORDING_START : DESIRED_RECORDING_STOP;
    int dispatched = 0;

    if (cJSON_IsNumber(slot_item)) {
        int slot = (int)cJSON_GetNumberValue(slot_item);
        camera_manager_set_desired_recording_slot(slot, intent);
        dispatched = 1;
        ESP_LOGI(TAG, "shutter %s → slot %d", record ? "start" : "stop", slot);
    } else {
        camera_manager_set_desired_recording_all(intent);
        dispatched = camera_manager_get_slot_count();
        ESP_LOGI(TAG, "shutter %s → all (%d slots)",
                 record ? "start" : "stop", dispatched);
    }

    cJSON_Delete(root);

    char resp[40];
    snprintf(resp, sizeof(resp), "{\"dispatched\":%d}", dispatched);
    send_json(req, resp);
    return ESP_OK;
}

/* ---- POST /api/remove-camera --------------------------------------------- */

static esp_err_t handler_remove_camera(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");
    if (!cJSON_IsNumber(slot_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'slot'");
        return ESP_FAIL;
    }
    int slot = (int)cJSON_GetNumberValue(slot_item);
    cJSON_Delete(root);

    esp_err_t err = camera_manager_remove_slot(slot);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "removed camera slot %d", slot);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- POST /api/reorder-cameras ------------------------------------------- */

static esp_err_t handler_reorder_cameras(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *order_arr = cJSON_GetObjectItem(root, "order");
    if (!cJSON_IsArray(order_arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'order' array");
        return ESP_FAIL;
    }

    int count = cJSON_GetArraySize(order_arr);
    int new_order[CAMERA_MAX_SLOTS];
    if (count <= 0 || count > CAMERA_MAX_SLOTS) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid order length");
        return ESP_FAIL;
    }

    for (int i = 0; i < count; i++) {
        cJSON *el = cJSON_GetArrayItem(order_arr, i);
        if (!cJSON_IsNumber(el)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "non-numeric in order");
            return ESP_FAIL;
        }
        new_order[i] = (int)cJSON_GetNumberValue(el);
    }
    cJSON_Delete(root);

    esp_err_t err = camera_manager_reorder_slots(new_order, count);
    if (err == ESP_ERR_INVALID_STATE) {
        /* 409 Conflict — a camera in the reorder set is currently connected. */
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"error\":\"camera connected — disconnect before reordering\"}");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid order");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "slots reordered (%d entries)", count);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- GET /api/cameras (BLE discovery) ------------------------------------ */

static esp_err_t handler_cameras(httpd_req_t *req)
{
    gopro_device_t devices[GOPRO_DISC_MAX];
    int n = open_gopro_ble_get_discovered(devices, GOPRO_DISC_MAX);

    /* Max entry: ~100 bytes; 10 entries + brackets = ~1100. */
    const size_t BUF_SIZE = 1280;
    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t pos = 0;
    buf[pos++] = '[';

    for (int i = 0; i < n; i++) {
        char addr_str[18];
        format_mac(addr_str, devices[i].addr.val);

        int written = snprintf(buf + pos, BUF_SIZE - pos,
            "%s{\"name\":\"%s\",\"addr\":\"%s\",\"addr_type\":%d,\"rssi\":%d}",
            (i == 0) ? "" : ",",
            devices[i].name,
            addr_str,
            devices[i].addr.type,
            devices[i].rssi);

        if (written < 0 || (size_t)written >= BUF_SIZE - pos) break;
        pos += (size_t)written;
    }

    if (pos < BUF_SIZE - 1) buf[pos++] = ']';
    buf[pos] = '\0';

    send_json(req, buf);
    free(buf);
    return ESP_OK;
}

/* ---- POST /api/scan ------------------------------------------------------ */

static esp_err_t handler_scan(httpd_req_t *req)
{
    open_gopro_ble_start_discovery();
    ESP_LOGI(TAG, "BLE scan started");
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- POST /api/scan-cancel ----------------------------------------------- */

static esp_err_t handler_scan_cancel(httpd_req_t *req)
{
    open_gopro_ble_stop_discovery();
    ESP_LOGI(TAG, "BLE scan cancelled");
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- POST /api/pair ------------------------------------------------------ */

static esp_err_t handler_pair(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *addr_item      = cJSON_GetObjectItem(root, "addr");
    cJSON *addr_type_item = cJSON_GetObjectItem(root, "addr_type");

    if (!cJSON_IsString(addr_item) || !cJSON_IsNumber(addr_type_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr or addr_type");
        return ESP_FAIL;
    }

    ble_addr_t ble_addr;
    ble_addr.type = (uint8_t)cJSON_GetNumberValue(addr_type_item);

    if (!parse_mac(cJSON_GetStringValue(addr_item), ble_addr.val)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid addr");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    open_gopro_ble_connect_by_addr(&ble_addr);
    ESP_LOGI(TAG, "pairing initiated");
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_cameras_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/paired-cameras",   .method = HTTP_GET,  .handler = handler_paired_cameras  },
        { .uri = "/api/shutter",          .method = HTTP_POST, .handler = handler_shutter         },
        { .uri = "/api/remove-camera",    .method = HTTP_POST, .handler = handler_remove_camera   },
        { .uri = "/api/reorder-cameras",  .method = HTTP_POST, .handler = handler_reorder_cameras },
        { .uri = "/api/cameras",          .method = HTTP_GET,  .handler = handler_cameras         },
        { .uri = "/api/scan",             .method = HTTP_POST, .handler = handler_scan            },
        { .uri = "/api/scan-cancel",      .method = HTTP_POST, .handler = handler_scan_cancel     },
        { .uri = "/api/pair",             .method = HTTP_POST, .handler = handler_pair            },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

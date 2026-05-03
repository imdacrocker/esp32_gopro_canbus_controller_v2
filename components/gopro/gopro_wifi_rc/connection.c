/*
 * connection.c — Station lifecycle handlers, HTTP probe, and per-slot
 * keepalive / WoL-retry timer management.  All functions here run on the
 * work task unless otherwise noted.
 *
 * §17.5, §17.6, §18.2 of camera_manager_design.md.
 */

#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_manager.h"
#include "gopro_model.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/conn";

/* ---- Internal helpers ---------------------------------------------------- */

static int find_managed_slot(const uint8_t mac[6])
{
    int slot = camera_manager_find_by_mac(mac);
    if (slot < 0) return -1;
    if (!gopro_model_uses_rc_emulation(camera_manager_get_model(slot))) return -1;
    return slot;
}

/* ---- Per-slot keepalive timer -------------------------------------------- */

static void keepalive_timer_cb(void *arg)
{
    int slot = (int)(intptr_t)arg;
    rc_work_cmd_t cmd = { .type = RC_CMD_KEEPALIVE_TICK,
                          .slot_cmd = { .slot = slot } };
    xQueueSend(s_work_queue, &cmd, 0);
}

void rc_arm_keepalive_timer(gopro_wifi_rc_ctx_t *ctx)
{
    if (ctx->keepalive_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = keepalive_timer_cb,
            .arg      = (void *)(intptr_t)ctx->slot,
            .name     = "rc_keepalive",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &ctx->keepalive_timer));
    }
    /* Stop first so we can call start even if already running. */
    esp_timer_stop(ctx->keepalive_timer);
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->keepalive_timer,
                                             (uint64_t)RC_KEEPALIVE_INTERVAL_MS * 1000));
}

void rc_disarm_keepalive_timer(gopro_wifi_rc_ctx_t *ctx)
{
    if (ctx->keepalive_timer) {
        esp_timer_stop(ctx->keepalive_timer);
    }
}

/* ---- Per-slot WoL retry timer -------------------------------------------- */

static void wol_retry_timer_cb(void *arg)
{
    int slot = (int)(intptr_t)arg;
    rc_work_cmd_t cmd = { .type = RC_CMD_WOL_RETRY,
                          .slot_cmd = { .slot = slot } };
    xQueueSend(s_work_queue, &cmd, 0);
}

void rc_arm_wol_retry_timer(gopro_wifi_rc_ctx_t *ctx)
{
    if (ctx->wol_retry_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = wol_retry_timer_cb,
            .arg      = (void *)(intptr_t)ctx->slot,
            .name     = "rc_wol_retry",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &ctx->wol_retry_timer));
    }
    esp_timer_stop(ctx->wol_retry_timer);
    ESP_ERROR_CHECK(esp_timer_start_periodic(ctx->wol_retry_timer,
                                             (uint64_t)RC_WOL_RETRY_INTERVAL_MS * 1000));
}

void rc_disarm_wol_retry_timer(gopro_wifi_rc_ctx_t *ctx)
{
    if (ctx->wol_retry_timer) {
        esp_timer_stop(ctx->wol_retry_timer);
    }
}

/* ---- Station event handlers ---------------------------------------------- */

void rc_handle_station_associated(const uint8_t mac[6])
{
    int slot = find_managed_slot(mac);
    if (slot < 0) return; /* Unknown or non-RC MAC — ignore. */

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];

    uint32_t ip = camera_manager_get_last_ip(slot);
    if (ip == 0) {
        /* No DHCP lease yet; CMD_STATION_DHCP will follow when the camera wakes. */
        ESP_LOGD(TAG, "slot %d: associated with no cached IP — waiting for DHCP", slot);
        return;
    }

    /* Camera is associating with a known cached IP (reuse from previous session).
     * It may be asleep; send WoL and arm keepalive. */
    ESP_LOGI(TAG, "slot %d: associated (no DHCP), sending WoL burst", slot);
    rc_send_wol(ip, ctx->mac);
    rc_arm_keepalive_timer(ctx);
}

void rc_handle_station_dhcp(const uint8_t mac[6], uint32_t ip)
{
    int slot = find_managed_slot(mac);
    if (slot < 0) return;

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    ctx->last_ip = ip;
    memcpy(ctx->mac, mac, 6); /* Keep MAC in sync (also set by driver add flow). */
    camera_manager_save_slot(slot);

    rc_arm_keepalive_timer(ctx);

    rc_work_cmd_t cmd = { .type = RC_CMD_PROBE, .slot_cmd = { .slot = slot } };
    xQueueSend(s_work_queue, &cmd, 0);

    ESP_LOGI(TAG, "slot %d: DHCP ip=%lu — probing", slot, (unsigned long)ip);
}

void rc_handle_station_disconnected(const uint8_t mac[6])
{
    int slot = find_managed_slot(mac);
    if (slot < 0) return;

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    rc_disarm_keepalive_timer(ctx);
    rc_disarm_wol_retry_timer(ctx);
    ctx->wifi_ready       = false;
    ctx->recording_status = CAMERA_RECORDING_UNKNOWN;
    camera_manager_on_wifi_disconnected(slot);

    ESP_LOGI(TAG, "slot %d: disassociated", slot);
}

/* ---- HTTP probe ---------------------------------------------------------- */

/*
 * Try GET /gp/gpControl/status up to RC_PROBE_RETRIES times.
 * On success: update camera name, mark wifi_ready, call
 * camera_manager_on_camera_ready(), send date/time best-effort.
 * On failure: log and return; next on_station_dhcp will retry.
 */
void rc_handle_probe(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];

    char body[RC_HTTP_STATUS_RESP_MAX];
    int  code = -1;

    for (int attempt = 0; attempt < RC_PROBE_RETRIES; attempt++) {
        ESP_LOGI(TAG, "slot %d: probe attempt %d/%d",
                 slot, attempt + 1, RC_PROBE_RETRIES);
        code = rc_http_get(ctx->last_ip, RC_HTTP_PATH_STATUS, body, sizeof(body));
        if (code == 200) break;
        if (attempt + 1 < RC_PROBE_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(RC_PROBE_RETRY_MS));
        }
    }

    if (code != 200) {
        ESP_LOGW(TAG, "slot %d: probe failed (last code=%d)", slot, code);
        return;
    }

    /* Parse camera name from field RC_STATUS_JSON_NAME inside "status" object.
     * TODO(hardware): verify field "30" is camera name on Hero4; may need to
     * call /gp/gpControl/info instead. */
    const char *name_key = "\"" RC_STATUS_JSON_NAME "\":";
    const char *p = strstr(body, name_key);
    if (p) {
        p += strlen(name_key);
        while (*p == ' ') p++;
        if (*p == '"') {
            p++;
            char name[32] = {0};
            int  i        = 0;
            while (*p && *p != '"' && i < (int)sizeof(name) - 1) {
                name[i++] = *p++;
            }
            if (i > 0) {
                camera_manager_set_name(slot, name);
            }
        }
    }

    ctx->wifi_ready = true;
    camera_manager_on_camera_ready(slot);
    ESP_LOGI(TAG, "slot %d: probe OK — wifi ready", slot);

    /* Date/time — best-effort; only if system clock has been set by can_manager. */
    rc_send_datetime(slot);
}

/* ---- Keepalive tick handler ---------------------------------------------- */

void rc_handle_keepalive_tick(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];

    rc_send_keepalive(ctx->last_ip);

    TickType_t now     = xTaskGetTickCount();
    TickType_t silence = now - ctx->last_keepalive_ack;

    if (ctx->last_keepalive_ack != 0 &&
        silence > pdMS_TO_TICKS(RC_KEEPALIVE_SILENCE_MS)) {
        /* No ACK for 10 s — camera may have gone to sleep. */
        if (ctx->wol_retry_timer == NULL ||
            !esp_timer_is_active(ctx->wol_retry_timer)) {
            ESP_LOGW(TAG, "slot %d: keepalive silence %lu ms — arming WoL retry",
                     slot, (unsigned long)(silence * portTICK_PERIOD_MS));
            rc_arm_wol_retry_timer(ctx);
        }
    } else if (ctx->wol_retry_timer && esp_timer_is_active(ctx->wol_retry_timer)) {
        /* ACK is fresh — disarm WoL retry. */
        rc_disarm_wol_retry_timer(ctx);
        if (!ctx->wifi_ready) {
            rc_work_cmd_t cmd = { .type = RC_CMD_PROBE,
                                  .slot_cmd = { .slot = slot } };
            xQueueSend(s_work_queue, &cmd, 0);
        }
    }
}

/* ---- WoL retry handler --------------------------------------------------- */

void rc_handle_wol_retry(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    ESP_LOGD(TAG, "slot %d: WoL retry", slot);
    rc_send_wol(ctx->last_ip, ctx->mac);
    rc_send_keepalive(ctx->last_ip);
}

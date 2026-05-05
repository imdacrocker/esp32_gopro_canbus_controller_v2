/*
 * connection.c — Station lifecycle handlers, slot promotion, HTTP identify
 * probe, keepalive watchdog, and per-slot keepalive / WoL-retry timer
 * management.  All functions here run on the work task unless otherwise noted.
 *
 * §17.4, §17.5, §17.6 of camera_manager_design.md.
 */

#include <stdlib.h>
#include <string.h>
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
     * It may be asleep; send WoL, prime with a keepalive, and arm the timer. */
    ESP_LOGI(TAG, "slot %d: associated (no DHCP), sending WoL burst", slot);
    rc_send_wol(ip, ctx->mac);
    rc_send_keepalive(ip);
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

    /* Prime the camera with one keepalive + one status request so it responds
     * within ms instead of waiting for the next scheduled timer tick.  The RX
     * task will post CMD_PROMOTE on the first response. */
    rc_send_keepalive(ip);
    rc_send_st(ip);

    rc_arm_keepalive_timer(ctx);

    ESP_LOGI(TAG, "slot %d: DHCP ip=%lu — primed, waiting for first response",
             slot, (unsigned long)ip);
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
    /* identify_attempted is intentionally NOT cleared — once a probe has run
     * this firmware session, the slot's model is settled (HERO4_* on success,
     * HERO_LEGACY_RC on failure).  Re-running the probe on every reconnect
     * adds no new information and risks misclassifying a transient HTTP
     * failure as a model downgrade. */
    camera_manager_on_wifi_disconnected(slot);

    ESP_LOGI(TAG, "slot %d: disassociated", slot);
}

/* ---- Promotion (replaces probe) ------------------------------------------ */

/*
 * Posted by the UDP RX task when the first datagram arrives from a slot's IP
 * (keepalive ACK or `st` response).  Idempotent — duplicate posts are a no-op
 * because the first one flips wifi_ready.
 *
 * Triggers the one-shot HTTP identify probe (per session) and the best-effort
 * date/time set.  Neither is on the readiness path; if either fails, the
 * camera is still considered ready.
 */
void rc_handle_promote(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    if (ctx->wifi_ready) return;

    ctx->wifi_ready = true;
    camera_manager_on_camera_ready(slot);
    ESP_LOGI(TAG, "slot %d: promoted — camera ready", slot);

    if (!ctx->identify_attempted) {
        ctx->identify_attempted = true;
        rc_work_cmd_t cmd = { .type = RC_CMD_HTTP_IDENTIFY,
                              .slot_cmd = { .slot = slot } };
        xQueueSend(s_work_queue, &cmd, 0);
    }

    /* Date/time is internally gated on gopro_model_supports_http_datetime() —
     * a no-op on the legacy fallback (HERO_LEGACY_RC). */
    rc_send_datetime(slot);
}

/* ---- HTTP identify probe ------------------------------------------------- */

/*
 * One-shot blocking HTTP GET /gp/gpControl, single attempt, RC_HTTP_TIMEOUT_MS
 * timeout.  Runs on the work task; the slot is already wifi_ready by the time
 * we get here, so a 2 s blocking call is acceptable — the RX task continues
 * receiving keepalive ACKs / `st` responses on its own task.
 *
 * On success: extract info.model_name / info.model_number / info.firmware_version
 * from the JSON body and persist them to the slot.  Logs all three at INFO so
 * unrecognised models can be added to gopro_model_from_name() over time.
 *
 * On failure (RST, timeout, non-200, parse fail): mark the slot as
 * CAMERA_MODEL_GOPRO_HERO_LEGACY_RC and persist.  Hero3-class cameras don't
 * run an HTTP server on their STA interface — this is the expected legacy path.
 */
void rc_handle_http_identify(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];
    if (ctx->last_ip == 0) {
        ESP_LOGW(TAG, "slot %d: identify skipped — no IP", slot);
        return;
    }

    char *body = malloc(RC_HTTP_IDENTIFY_RESP_MAX);
    if (!body) {
        ESP_LOGW(TAG, "slot %d: identify malloc(%d) failed",
                 slot, RC_HTTP_IDENTIFY_RESP_MAX);
        return;
    }

    int code = rc_http_get(ctx->last_ip, RC_HTTP_PATH_IDENTIFY,
                           RC_HTTP_TIMEOUT_MS,
                           body, RC_HTTP_IDENTIFY_RESP_MAX);

    char model_name[32]   = {0};
    int  model_number     = -1;
    char firmware[32]     = {0};

    bool parsed = (code == 200) &&
                  rc_parse_identify_json(body,
                                          model_name,   sizeof(model_name),
                                          &model_number,
                                          firmware,     sizeof(firmware));

    if (parsed) {
        ESP_LOGI(TAG, "slot %d: identify OK — model='%s' model_num=%d fw='%s'",
                 slot, model_name, model_number, firmware);

        camera_model_t mapped = gopro_model_from_name(model_name);
        camera_manager_set_model(slot, mapped);
        if (model_name[0] != '\0') {
            camera_manager_set_name(slot, model_name);
        }
        camera_manager_save_slot(slot);
    } else {
        ESP_LOGI(TAG, "slot %d: identify failed (code=%d) — legacy RC camera",
                 slot, code);
        camera_manager_set_model(slot, CAMERA_MODEL_GOPRO_HERO_LEGACY_RC);
        camera_manager_save_slot(slot);
    }

    free(body);
}

/* ---- Keepalive tick handler ---------------------------------------------- */

/*
 * Send a keepalive every RC_KEEPALIVE_INTERVAL_MS, then check the silence
 * window: if no UDP datagram (ACK or `st` response) has arrived from the camera
 * for RC_KEEPALIVE_SILENCE_MS, arm the WoL retry timer.  When traffic resumes
 * the RX task refreshes last_response_tick and the next tick disarms the timer.
 */
void rc_handle_keepalive_tick(int slot)
{
    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];

    rc_send_keepalive(ctx->last_ip);

    TickType_t now     = xTaskGetTickCount();
    TickType_t silence = now - ctx->last_response_tick;

    bool silent = (ctx->last_response_tick != 0 &&
                   silence > pdMS_TO_TICKS(RC_KEEPALIVE_SILENCE_MS));

    if (silent) {
        if (ctx->wol_retry_timer == NULL ||
            !esp_timer_is_active(ctx->wol_retry_timer)) {
            ESP_LOGW(TAG, "slot %d: silence %lu ms — arming WoL retry",
                     slot, (unsigned long)(silence * portTICK_PERIOD_MS));
            rc_arm_wol_retry_timer(ctx);
        }
    } else if (ctx->wol_retry_timer && esp_timer_is_active(ctx->wol_retry_timer)) {
        rc_disarm_wol_retry_timer(ctx);
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

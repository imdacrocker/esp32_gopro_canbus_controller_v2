/*
 * notify.c — ATT notification routing.
 *
 * The ble_core on_notify_rx callback fires with (conn_handle, attr_handle,
 * data, len) for every incoming ATT notification or indication.  This file
 * maps attr_handle to the correct gopro_channel_t and feeds the data into
 * the per-slot GPBS reassembler (query.c).
 *
 * All code runs on the NimBLE host task (core 1).
 */

#include "esp_log.h"
#include "open_gopro_ble_internal.h"

static const char *TAG = "gopro_ble/notify";

void gopro_notify_rx(uint16_t conn_handle, uint16_t attr_handle,
                     const uint8_t *data, uint16_t len)
{
    gopro_ble_ctx_t *ctx = gopro_ctx_by_conn(conn_handle);
    if (!ctx) {
        /* Notification from an unknown connection — ignore. */
        return;
    }

    const gopro_gatt_handles_t *g = &ctx->gatt;
    gopro_channel_t chan;

    const char *chan_name;
    if (attr_handle == g->cmd_resp_notify) {
        chan = GOPRO_CHAN_CMD;       chan_name = "cmd";
    } else if (attr_handle == g->settings_resp_notify) {
        chan = GOPRO_CHAN_SETTINGS;  chan_name = "settings";
    } else if (attr_handle == g->query_resp_notify) {
        chan = GOPRO_CHAN_QUERY;     chan_name = "query";
    } else {
        ESP_LOGW(TAG, "slot %d: notify on unregistered handle 0x%04x len=%d",
                 ctx->slot, attr_handle, len);
        return;
    }

    ESP_LOGD(TAG, "slot %d: notify on %s (h=0x%04x) len=%d",
             ctx->slot, chan_name, attr_handle, len);

    gopro_query_feed(ctx, chan, data, len);
}

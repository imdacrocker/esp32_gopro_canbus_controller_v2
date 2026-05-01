/*
 * gatt.c — MTU negotiation, service/characteristic discovery, CCCD subscription.
 *
 * Runs entirely on the NimBLE host task.  All operations are chained through
 * NimBLE callbacks; nothing blocks.
 *
 * Flow:
 *   gopro_gatt_start_discovery()
 *     -> ble_gattc_disc_all_chrs()   [over 1..0xFFFF to catch all services]
 *     -> on_chr_disc()               [match each chr UUID to gatt handle table]
 *     -> gopro_gatt_subscribe_all()  [when BLE_HS_EDONE]
 *     -> on_cccd_write()             [sequential CCCD writes, one per notify/indicate chr]
 *     -> gopro_readiness_start()     [when all CCCDs written]
 */

#include <string.h>
#include "esp_log.h"
#include "host/ble_gatt.h"
#include "open_gopro_ble_internal.h"

static const char *TAG = "gopro_ble/gatt";

/* Forward declaration — defined later in this file. */
static void gopro_gatt_write_next_cccd(gopro_ble_ctx_t *ctx);

/* ---- Known GoPro characteristic UUID table ------------------------------- */

/*
 * Maps each GoPro characteristic to the byte offset of its handle field
 * inside gopro_gatt_handles_t.  Used by on_chr_disc() to populate the table.
 */
typedef struct {
    ble_uuid128_t uuid;
    uint16_t      offset;   /* offsetof(gopro_gatt_handles_t, field) */
} chr_map_entry_t;

#define HANDLE_OFF(field)  ((uint16_t)offsetof(gopro_gatt_handles_t, field))

static const chr_map_entry_t k_chr_map[] = {
    { GOPRO_CHR_CMD_WRITE_UUID,       HANDLE_OFF(cmd_write)             },
    { GOPRO_CHR_CMD_RESP_NOTIFY_UUID, HANDLE_OFF(cmd_resp_notify)       },
    { GOPRO_CHR_SETTINGS_WRITE_UUID,  HANDLE_OFF(settings_write)        },
    { GOPRO_CHR_SETTINGS_RESP_UUID,   HANDLE_OFF(settings_resp_notify)  },
    { GOPRO_CHR_QUERY_WRITE_UUID,     HANDLE_OFF(query_write)           },
    { GOPRO_CHR_QUERY_RESP_NOTIFY_UUID, HANDLE_OFF(query_resp_notify)   },
    { GOPRO_CHR_NET_MGMT_CMD_UUID,    HANDLE_OFF(net_mgmt_cmd_write)    },
    { GOPRO_CHR_NET_MGMT_RESP_UUID,   HANDLE_OFF(net_mgmt_resp_notify)  },
    { GOPRO_CHR_WIFI_AP_PWR_UUID,     HANDLE_OFF(wifi_ap_pwr_write)     },
    { GOPRO_CHR_WIFI_AP_SSID_UUID,    HANDLE_OFF(wifi_ap_ssid_read)     },
    { GOPRO_CHR_WIFI_AP_PASS_UUID,    HANDLE_OFF(wifi_ap_pass_read)     },
    { GOPRO_CHR_WIFI_AP_STATE_UUID,   HANDLE_OFF(wifi_ap_state_indicate)},
};
#define CHR_MAP_LEN  (sizeof(k_chr_map) / sizeof(k_chr_map[0]))

/* ---- CCCD subscription list ---------------------------------------------- */

/*
 * Ordered list of (val_handle field offset, CCCD value) pairs.
 * Written sequentially: cmd_resp → settings_resp → query_resp → net_mgmt_resp
 * → wifi_ap_state (indicate).
 *
 * GoPro cameras reliably place the CCCD descriptor at val_handle + 1, which
 * is the standard BLE convention for single-descriptor characteristics.
 */
typedef struct {
    uint16_t handle_offset;  /* offset into gopro_gatt_handles_t */
    uint16_t cccd_val;
} subscr_entry_t;

static const subscr_entry_t k_subscr_list[] = {
    { HANDLE_OFF(cmd_resp_notify),      BLE_CCCD_NOTIFY   },
    { HANDLE_OFF(settings_resp_notify), BLE_CCCD_NOTIFY   },
    { HANDLE_OFF(query_resp_notify),    BLE_CCCD_NOTIFY   },
    { HANDLE_OFF(net_mgmt_resp_notify), BLE_CCCD_NOTIFY   },
    { HANDLE_OFF(wifi_ap_state_indicate), BLE_CCCD_INDICATE },
};
#define SUBSCR_LIST_LEN  (sizeof(k_subscr_list) / sizeof(k_subscr_list[0]))

/* ---- Per-slot subscription cursor ---------------------------------------- */

/* Tracks which subscription we're writing for a given slot. */
static int s_subscr_cur[CAMERA_MAX_SLOTS];

/* ---- Characteristic discovery callback ----------------------------------- */

static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;

    if (error->status == BLE_HS_EDONE) {
        /* All characteristics discovered — start CCCD subscriptions. */
        s_subscr_cur[ctx->slot] = 0;

        uint8_t *handles = (uint8_t *)&ctx->gatt;
        int missing = 0;
        for (int i = 0; i < (int)CHR_MAP_LEN; i++) {
            uint16_t h;
            memcpy(&h, handles + k_chr_map[i].offset, sizeof(h));
            if (h == 0) {
                ESP_LOGW(TAG, "slot %d: characteristic at offset %u not found",
                         ctx->slot, k_chr_map[i].offset);
                missing++;
            }
        }
        if (missing > 0) {
            ESP_LOGW(TAG, "slot %d: %d characteristic(s) missing", ctx->slot, missing);
        }

        /* Start writing CCCDs — forward declared below */
        gopro_gatt_write_next_cccd(ctx);
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "slot %d: chr discovery error 0x%04x", ctx->slot, error->status);
        ble_gap_terminate(ctx->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    /* Match UUID to our known table and record val_handle. */
    for (int i = 0; i < (int)CHR_MAP_LEN; i++) {
        if (ble_uuid_cmp(&chr->uuid.u, &k_chr_map[i].uuid.u) == 0) {
            uint8_t *handles = (uint8_t *)&ctx->gatt;
            memcpy(handles + k_chr_map[i].offset, &chr->val_handle, sizeof(uint16_t));
            ESP_LOGD(TAG, "slot %d: found chr offset=%u val_handle=0x%04x",
                     ctx->slot, k_chr_map[i].offset, chr->val_handle);
            break;
        }
    }
    return 0;
}

/* ---- CCCD write callback ------------------------------------------------- */

static int on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;

    if (error->status != 0) {
        /* Non-fatal: log and continue — camera may not have this chr. */
        ESP_LOGW(TAG, "slot %d: CCCD write error 0x%04x (continuing)",
                 ctx->slot, error->status);
    }

    s_subscr_cur[ctx->slot]++;

    gopro_gatt_write_next_cccd(ctx);
    return 0;
}

/* ---- Sequential CCCD writer ---------------------------------------------- */

static void gopro_gatt_write_next_cccd(gopro_ble_ctx_t *ctx)
{
    int cur = s_subscr_cur[ctx->slot];

    if (cur >= (int)SUBSCR_LIST_LEN) {
        /* All CCCDs written — proceed to readiness poll. */
        ESP_LOGI(TAG, "slot %d: all CCCDs subscribed", ctx->slot);
        gopro_readiness_start(ctx);
        return;
    }

    const subscr_entry_t *e = &k_subscr_list[cur];
    uint8_t *handles = (uint8_t *)&ctx->gatt;
    uint16_t val_handle;
    memcpy(&val_handle, handles + e->handle_offset, sizeof(uint16_t));

    if (val_handle == 0) {
        /* Characteristic not found on this camera — skip. */
        s_subscr_cur[ctx->slot]++;
        gopro_gatt_write_next_cccd(ctx);
        return;
    }

    uint16_t cccd_handle = val_handle + 1;  /* CCCD is at val_handle + 1 on GoPro */
    uint16_t cccd_val    = e->cccd_val;

    int rc = ble_gattc_write_flat(ctx->conn_handle, cccd_handle,
                                   &cccd_val, sizeof(cccd_val),
                                   on_cccd_write, ctx);
    if (rc != 0) {
        ESP_LOGW(TAG, "slot %d: CCCD write enqueue failed rc=%d, skipping",
                 ctx->slot, rc);
        s_subscr_cur[ctx->slot]++;
        gopro_gatt_write_next_cccd(ctx);
    }
}

/* ---- Entry point --------------------------------------------------------- */

void gopro_gatt_start_discovery(gopro_ble_ctx_t *ctx)
{
    memset(&ctx->gatt, 0, sizeof(ctx->gatt));
    s_subscr_cur[ctx->slot] = 0;

    ESP_LOGI(TAG, "slot %d: starting chr discovery mtu=%d",
             ctx->slot, ctx->negotiated_mtu);

    int rc = ble_gattc_disc_all_chrs(ctx->conn_handle,
                                      1, 0xFFFF,
                                      on_chr_disc, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "slot %d: ble_gattc_disc_all_chrs rc=%d", ctx->slot, rc);
        ble_gap_terminate(ctx->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

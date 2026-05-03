/*
 * driver.c — Component init, per-slot context table, and discovery list.
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "open_gopro_ble_internal.h"
#include "ble_core.h"
#include "gopro_model.h"

static const char *TAG = "gopro_ble";

/* ---- Per-slot context table ---------------------------------------------- */

static gopro_ble_ctx_t s_ctx[CAMERA_MAX_SLOTS];

gopro_ble_ctx_t *gopro_ctx_by_slot(int slot)
{
    if (slot < 0 || slot >= CAMERA_MAX_SLOTS) {
        return NULL;
    }
    return &s_ctx[slot];
}

gopro_ble_ctx_t *gopro_ctx_by_conn(uint16_t conn_handle)
{
    if (conn_handle == GOPRO_CONN_NONE) {
        return NULL;
    }
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        if (s_ctx[i].conn_handle == conn_handle) {
            return &s_ctx[i];
        }
    }
    return NULL;
}

/* ---- Discovery list (§15.2) ---------------------------------------------- */

static SemaphoreHandle_t s_disc_mutex;
static gopro_device_t    s_disc_list[GOPRO_DISC_MAX];
static int               s_disc_count;

/* 16-bit GoPro service UUID, used to filter advertisements. */
static const ble_uuid16_t k_gopro_svc_uuid = BLE_UUID16_INIT(GOPRO_SVC_UUID16);

static void on_disc_cb(ble_addr_t addr, int8_t rssi,
                       const uint8_t *data, int len)
{
    /* Filter: advertisement must contain the GoPro 16-bit service UUID. */
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, data, len) != 0) {
        return;
    }

    bool found = false;
    for (int i = 0; i < fields.num_uuids16; i++) {
        if (ble_uuid_cmp(&fields.uuids16[i].u, &k_gopro_svc_uuid.u) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    /* Already known? Update RSSI. */
    xSemaphoreTake(s_disc_mutex, portMAX_DELAY);
    for (int i = 0; i < s_disc_count; i++) {
        if (memcmp(&s_disc_list[i].addr, &addr, sizeof(ble_addr_t)) == 0) {
            s_disc_list[i].rssi = rssi;
            xSemaphoreGive(s_disc_mutex);
            return;
        }
    }

    if (s_disc_count < GOPRO_DISC_MAX) {
        gopro_device_t *d = &s_disc_list[s_disc_count++];
        d->addr = addr;
        d->rssi = rssi;
        /* Best-effort name extraction from complete or shortened local name. */
        const uint8_t *name   = NULL;
        uint8_t        namelen = 0;
        if (fields.name_len > 0) {
            name    = fields.name;
            namelen = fields.name_len;
        }
        if (name && namelen > 0) {
            size_t copy = namelen < sizeof(d->name) - 1 ? namelen : sizeof(d->name) - 1;
            memcpy(d->name, name, copy);
            d->name[copy] = '\0';
        } else {
            snprintf(d->name, sizeof(d->name), "GoPro-%02X%02X",
                     addr.val[1], addr.val[0]);
        }
        ESP_LOGI(TAG, "disc: %s rssi=%d", d->name, rssi);
    }
    xSemaphoreGive(s_disc_mutex);
}

void open_gopro_ble_start_discovery(void)
{
    xSemaphoreTake(s_disc_mutex, portMAX_DELAY);
    s_disc_count = 0;
    memset(s_disc_list, 0, sizeof(s_disc_list));
    xSemaphoreGive(s_disc_mutex);

    ble_core_start_discovery(120000 /* 120 s */);
}

void open_gopro_ble_stop_discovery(void)
{
    ble_core_stop_discovery();
}

int open_gopro_ble_get_discovered(gopro_device_t *out, int max_count)
{
    xSemaphoreTake(s_disc_mutex, portMAX_DELAY);
    int n = s_disc_count < max_count ? s_disc_count : max_count;
    memcpy(out, s_disc_list, n * sizeof(gopro_device_t));
    xSemaphoreGive(s_disc_mutex);
    return n;
}

/* ---- Connection ---------------------------------------------------------- */

void open_gopro_ble_connect_by_addr(const ble_addr_t *addr)
{
    ble_core_connect_by_addr(addr);
}

/* ---- UTC sync (§15.5) ---------------------------------------------------- */

void open_gopro_ble_sync_time_all(void)
{
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        gopro_ble_ctx_t *ctx = &s_ctx[i];
        if (ctx->conn_handle == GOPRO_CONN_NONE) {
            continue;
        }
        gopro_control_set_datetime(ctx);
        if (ctx->cohn_pending_utc) {
            ctx->cohn_pending_utc = false;
            gopro_cohn_provision(ctx);
        }
    }
}

/* ---- Re-provisioning ----------------------------------------------------- */

void open_gopro_ble_reprovision(int slot)
{
    gopro_ble_ctx_t *ctx = gopro_ctx_by_slot(slot);
    if (!ctx || ctx->conn_handle == GOPRO_CONN_NONE) {
        ESP_LOGW(TAG, "reprovision slot %d: not connected", slot);
        return;
    }
    ESP_LOGI(TAG, "reprovision slot %d", slot);
    gopro_cohn_provision(ctx);
}

/* ---- COHN credentials ---------------------------------------------------- */

bool open_gopro_ble_get_cohn_credentials(int slot,
                                          char *user_out, size_t user_len,
                                          char *pass_out, size_t pass_len)
{
    /* Implemented in cohn.c — forward declaration for linker, but cohn.c
     * exports gopro_cohn_load() which is the actual NVS reader.
     * This shim calls it and copies into the caller's buffers. */
    extern bool gopro_cohn_load(int slot, char *user, size_t ulen,
                                char *pass, size_t plen);
    return gopro_cohn_load(slot, user_out, user_len, pass_out, pass_len);
}

/* ---- Component init (§15.9) ---------------------------------------------- */

/* Forward declarations for callbacks defined in pairing.c and notify.c */
extern void gopro_on_connected(uint16_t conn_handle, ble_addr_t addr);
extern void gopro_on_encrypted(uint16_t conn_handle, ble_addr_t addr);
extern void gopro_on_disconnected(uint16_t conn_handle, ble_addr_t addr,
                                   uint8_t reason);

void open_gopro_ble_init(void)
{
    /* Initialise context table */
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        memset(&s_ctx[i], 0, sizeof(gopro_ble_ctx_t));
        s_ctx[i].conn_handle = GOPRO_CONN_NONE;
        s_ctx[i].slot        = i;
    }

    s_disc_mutex = xSemaphoreCreateMutex();
    configASSERT(s_disc_mutex);

    /* Register BLE event callbacks */
    ble_core_callbacks_t cbs = {
        .on_disc                   = on_disc_cb,
        .on_connected              = gopro_on_connected,
        .on_encrypted              = gopro_on_encrypted,
        .on_disconnected           = gopro_on_disconnected,
        .on_notify_rx              = gopro_notify_rx,
        .is_known_addr             = camera_manager_is_known_ble_addr,
        .has_disconnected_cameras  = camera_manager_has_disconnected_cameras,
    };
    ble_core_register_callbacks(&cbs);

    /* TODO: build known-address list from camera_manager NVS records and
     * call ble_core_purge_unknown_bonds() once address-type is persisted. */

    ESP_LOGI(TAG, "init OK");
}

/*
 * cohn.c — COHN provisioning sequence and NVS credential storage.
 *
 * Sequence (§15.6):
 *   1. RequestCreateCOHNCert  → camera generates TLS cert (we skip reading it)
 *   2. RequestSetApEntries    → give camera our SoftAP SSID + open security
 *   3. (camera auto-connects to SoftAP; wifi_manager fires on_station_ip)
 *   4. RequestGetCOHNStatus   → poll until status == CONNECTED, extract creds
 *   5. Save creds to NVS cam_N/gopro_cohn
 *   6. camera_manager_set_camera_ready() → triggers open_gopro_http probe
 *
 * All writes go to net_mgmt_cmd_write (GP-0091).
 * All responses arrive on net_mgmt_resp_notify (GP-0092) → routed here via query.c.
 *
 * Protobuf encoding is hand-rolled (no protobuf library dependency).
 * Field tag = (field_number << 3) | wire_type.
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/network_management.html
 */

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "open_gopro_ble_internal.h"

static const char *TAG = "gopro_ble/cohn";

/* ---- NVS key layout (§15.8) ---------------------------------------------- */

/* NVS namespace pattern: "cam_N" where N is the slot index. */
#define NVS_GOPRO_COHN_KEY  "gopro_cohn"

typedef struct {
    char cohn_user[32];
    char cohn_pass[64];
} gopro_cohn_nv_record_t;

/* ---- Per-slot COHN poll state -------------------------------------------- */

typedef enum {
    COHN_STEP_IDLE         = 0,
    COHN_STEP_CREATE_CERT,
    COHN_STEP_SET_AP,
    COHN_STEP_GET_STATUS,
} cohn_step_t;

typedef struct {
    cohn_step_t        step;
    uint8_t            poll_count;
    esp_timer_handle_t poll_timer;
} cohn_state_t;

static cohn_state_t s_cohn[CAMERA_MAX_SLOTS];

/* ---- Protobuf helpers ----------------------------------------------------- */

/* Append a varint-encoded field to buf[pos]. Returns new pos. */
static int pb_write_varint_field(uint8_t *buf, int pos, uint8_t tag, uint32_t val)
{
    buf[pos++] = tag;
    /* Single-byte varint encoding (all values used here fit in 7 bits). */
    buf[pos++] = (uint8_t)(val & 0x7Fu);
    return pos;
}

/* Append a length-delimited string field. Returns new pos. */
static int pb_write_string_field(uint8_t *buf, int pos, uint8_t tag,
                                  const char *str, uint8_t slen)
{
    buf[pos++] = tag;
    buf[pos++] = slen;
    memcpy(buf + pos, str, slen);
    return pos + slen;
}

/* ---- Packet builders ----------------------------------------------------- */

/*
 * Build a GPBS-framed protobuf command for the net_mgmt channel.
 * payload: feature_marker(1) + feature_id(1) + action_id(1) + pb_body
 */
static int build_net_mgmt_pkt(uint8_t *pkt, uint8_t action_id,
                                const uint8_t *pb_body, uint8_t pb_len)
{
    uint8_t payload[64];
    int     plen = 0;
    payload[plen++] = GOPRO_PROTO_CMD_MARKER;
    payload[plen++] = GOPRO_PROTO_FEATURE_NET_MGMT;
    payload[plen++] = action_id;
    if (pb_body && pb_len > 0) {
        memcpy(payload + plen, pb_body, pb_len);
        plen += pb_len;
    }

    int hdr_len;
    if ((uint8_t)plen <= GPBS_HDR_GENERAL_MAX) {
        hdr_len = gpbs_write_hdr(pkt, (uint8_t)plen);
    } else {
        hdr_len = gpbs_write_hdr13(pkt, (uint16_t)plen);
    }
    memcpy(pkt + hdr_len, payload, plen);
    return hdr_len + plen;
}

/* ---- COHN step senders --------------------------------------------------- */

#define COHN_STATUS_POLL_INTERVAL_MS  2000u
#define COHN_STATUS_POLL_MAX          15u  /* ~30 s total */

static void send_create_cert(gopro_ble_ctx_t *ctx)
{
    uint8_t pkt[8];
    /* RequestCreateCOHNCert: no protobuf body */
    int len = build_net_mgmt_pkt(pkt, GOPRO_COHN_ACTION_CREATE_CERT, NULL, 0);
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.net_mgmt_cmd_write, pkt, len);
    ESP_LOGI(TAG, "slot %d: → RequestCreateCOHNCert", ctx->slot);
}

static void send_set_ap(gopro_ble_ctx_t *ctx)
{
    wifi_config_t wc;
    char ssid[32] = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &wc) == ESP_OK) {
        snprintf(ssid, sizeof(ssid), "%s", (char *)wc.ap.ssid);
    }

    uint8_t pb[48];
    int ppos = 0;
    ppos = pb_write_string_field(pb, ppos, GOPRO_SETAP_PB_SSID_TAG,
                                  ssid, (uint8_t)strlen(ssid));
    /* No password (open AP): omit field 2 */
    ppos = pb_write_varint_field(pb, ppos, GOPRO_SETAP_PB_SECTYPE_TAG,
                                  GOPRO_SETAP_SECURITY_OPEN);

    uint8_t pkt[64];
    int len = build_net_mgmt_pkt(pkt, GOPRO_COHN_ACTION_SET_AP, pb, (uint8_t)ppos);
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.net_mgmt_cmd_write, pkt, len);
    ESP_LOGI(TAG, "slot %d: → RequestSetApEntries ssid=%s", ctx->slot, ssid);
}

static void send_get_status(gopro_ble_ctx_t *ctx)
{
    uint8_t pkt[8];
    int len = build_net_mgmt_pkt(pkt, GOPRO_COHN_ACTION_GET_STATUS, NULL, 0);
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.net_mgmt_cmd_write, pkt, len);
}

/* ---- Status poll timer --------------------------------------------------- */

static void on_cohn_poll_timer(void *arg)
{
    gopro_ble_ctx_t *ctx = (gopro_ble_ctx_t *)arg;
    cohn_state_t    *cs  = &s_cohn[ctx->slot];

    if (cs->step != COHN_STEP_GET_STATUS) {
        return;
    }
    if (ctx->conn_handle == GOPRO_CONN_NONE) {
        return;
    }

    cs->poll_count++;
    if (cs->poll_count > COHN_STATUS_POLL_MAX) {
        ESP_LOGE(TAG, "slot %d: COHN status poll timed out", ctx->slot);
        cs->step = COHN_STEP_IDLE;
        ctx->cohn_provisioning = false;
        esp_timer_stop(cs->poll_timer);
        return;
    }
    send_get_status(ctx);
    ESP_LOGD(TAG, "slot %d: COHN poll %d/%d", ctx->slot,
             cs->poll_count, COHN_STATUS_POLL_MAX);
}

/* ---- Response handler (called from query.c) ------------------------------ */

/*
 * Simple protobuf varint decoder — reads a single-byte varint.
 * Returns value or 0xFF on error.
 */
static uint8_t pb_read_varint(const uint8_t *buf, uint16_t len, uint16_t *pos)
{
    if (*pos >= len) {
        return 0xFF;
    }
    return buf[(*pos)++];
}

/*
 * Read a length-prefixed string field from buf at *pos.
 * Copies up to out_max-1 bytes into out and NUL-terminates.
 */
static void pb_read_string(const uint8_t *buf, uint16_t len, uint16_t *pos,
                            char *out, size_t out_max)
{
    if (*pos >= len) {
        out[0] = '\0';
        return;
    }
    uint8_t slen = buf[(*pos)++];
    if (*pos + slen > len) {
        slen = len - *pos;
    }
    size_t copy = slen < out_max - 1 ? slen : out_max - 1;
    memcpy(out, buf + *pos, copy);
    out[copy] = '\0';
    *pos += slen;
}

void gopro_cohn_on_response(gopro_ble_ctx_t *ctx, uint8_t action_id,
                             const uint8_t *data, uint16_t len)
{
    cohn_state_t *cs = &s_cohn[ctx->slot];

    if (len < GOPRO_NMGMT_RESP_HDR_LEN) {
        ESP_LOGW(TAG, "slot %d: short net_mgmt response len=%d", ctx->slot, len);
        return;
    }

    uint8_t result = data[GOPRO_NMGMT_RESP_RESULT_IDX];
    if (result != GOPRO_COHN_RESULT_OK) {
        ESP_LOGE(TAG, "slot %d: COHN action 0x%02x failed result=0x%02x",
                 ctx->slot, action_id, result);
        cs->step = COHN_STEP_IDLE;
        ctx->cohn_provisioning = false;
        return;
    }

    switch (action_id) {
    case GOPRO_COHN_RESP_CREATE_CERT:
        ESP_LOGI(TAG, "slot %d: cert created, sending SetApEntries", ctx->slot);
        cs->step = COHN_STEP_SET_AP;
        send_set_ap(ctx);
        break;

    case GOPRO_COHN_RESP_SET_AP:
        ESP_LOGI(TAG, "slot %d: AP entries set, starting status poll", ctx->slot);
        cs->step       = COHN_STEP_GET_STATUS;
        cs->poll_count = 0;
        send_get_status(ctx);
        esp_timer_start_periodic(cs->poll_timer,
                                  COHN_STATUS_POLL_INTERVAL_MS * 1000ULL);
        break;

    case GOPRO_COHN_RESP_GET_STATUS: {
        /* Parse protobuf response body (after 4-byte header). */
        const uint8_t *pb  = data + GOPRO_NMGMT_RESP_HDR_LEN;
        uint16_t       plen = len - GOPRO_NMGMT_RESP_HDR_LEN;
        uint16_t       pos  = 0;

        uint8_t  status = 0;
        char     user[32] = {0};
        char     pass[64] = {0};
        bool     got_status = false;

        while (pos < plen) {
            uint8_t tag = pb_read_varint(pb, plen, &pos);
            if (tag == GOPRO_COHN_PB_STATUS_TAG) {
                status     = pb_read_varint(pb, plen, &pos);
                got_status = true;
            } else if (tag == GOPRO_COHN_PB_USER_TAG) {
                pb_read_string(pb, plen, &pos, user, sizeof(user));
            } else if (tag == GOPRO_COHN_PB_PASS_TAG) {
                pb_read_string(pb, plen, &pos, pass, sizeof(pass));
            } else {
                /* Skip unknown field: read and discard next varint or length. */
                uint8_t wire = tag & 0x07u;
                if (wire == 0) {
                    pb_read_varint(pb, plen, &pos);        /* varint */
                } else if (wire == 2) {
                    uint8_t skip = pb_read_varint(pb, plen, &pos);
                    pos += skip;                           /* length-delimited */
                } else {
                    pos = plen;                            /* unknown — give up */
                }
            }
        }

        if (!got_status || status != GOPRO_COHN_STATUS_CONNECTED) {
            /* Not yet connected — timer will retry. */
            ESP_LOGD(TAG, "slot %d: COHN status=%d (not connected yet)", ctx->slot, status);
            break;
        }

        /* Connected — stop poll timer and save credentials. */
        esp_timer_stop(cs->poll_timer);
        cs->step = COHN_STEP_IDLE;
        ctx->cohn_provisioning = false;

        ESP_LOGI(TAG, "slot %d: COHN connected, saving credentials", ctx->slot);
        extern bool gopro_cohn_save(int slot, const char *user, const char *pass);
        gopro_cohn_save(ctx->slot, user, pass);
        camera_manager_set_camera_ready(ctx->slot, true);
        break;
    }

    default:
        ESP_LOGW(TAG, "slot %d: unknown COHN response action 0x%02x",
                 ctx->slot, action_id);
        break;
    }
}

/* ---- NVS helpers ---------------------------------------------------------- */

static void make_ns(int slot, char *ns, size_t nslen)
{
    snprintf(ns, nslen, "cam_%d", slot);
}

bool gopro_cohn_load(int slot, char *user, size_t ulen,
                     char *pass, size_t plen)
{
    char ns[16];
    make_ns(slot, ns, sizeof(ns));

    nvs_handle_t hdl;
    if (nvs_open(ns, NVS_READONLY, &hdl) != ESP_OK) {
        return false;
    }

    gopro_cohn_nv_record_t rec;
    size_t sz = sizeof(rec);
    esp_err_t err = nvs_get_blob(hdl, NVS_GOPRO_COHN_KEY, &rec, &sz);
    nvs_close(hdl);

    if (err != ESP_OK || sz != sizeof(rec)) {
        return false;
    }

    snprintf(user, ulen, "%s", rec.cohn_user);
    snprintf(pass, plen, "%s", rec.cohn_pass);
    return (rec.cohn_user[0] != '\0' && rec.cohn_pass[0] != '\0');
}

bool gopro_cohn_save(int slot, const char *user, const char *pass)
{
    char ns[16];
    make_ns(slot, ns, sizeof(ns));

    gopro_cohn_nv_record_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.cohn_user, sizeof(rec.cohn_user), "%s", user);
    snprintf(rec.cohn_pass, sizeof(rec.cohn_pass), "%s", pass);

    nvs_handle_t hdl;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "slot %d: nvs_open failed: %s", slot, esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(hdl, NVS_GOPRO_COHN_KEY, &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(hdl);
    }
    nvs_close(hdl);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "slot %d: nvs write failed: %s", slot, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* ---- Entry point --------------------------------------------------------- */

void gopro_cohn_provision(gopro_ble_ctx_t *ctx)
{
    cohn_state_t *cs = &s_cohn[ctx->slot];

    if (ctx->cohn_provisioning) {
        ESP_LOGW(TAG, "slot %d: provision already in progress", ctx->slot);
        return;
    }
    if (ctx->conn_handle == GOPRO_CONN_NONE || ctx->gatt.net_mgmt_cmd_write == 0) {
        ESP_LOGE(TAG, "slot %d: cannot provision — not connected or no net_mgmt handle",
                 ctx->slot);
        return;
    }

    /* Clear stale credentials before re-provisioning. */
    char ns[16];
    make_ns(ctx->slot, ns, sizeof(ns));
    nvs_handle_t hdl;
    if (nvs_open(ns, NVS_READWRITE, &hdl) == ESP_OK) {
        nvs_erase_key(hdl, NVS_GOPRO_COHN_KEY);
        nvs_commit(hdl);
        nvs_close(hdl);
    }

    ctx->cohn_provisioning = true;
    cs->step               = COHN_STEP_CREATE_CERT;
    cs->poll_count         = 0;

    if (!cs->poll_timer) {
        esp_timer_create_args_t args = {
            .callback        = on_cohn_poll_timer,
            .arg             = ctx,
            .dispatch_method = ESP_TIMER_TASK,
            .name            = "cohn_poll",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &cs->poll_timer));
    }

    send_create_cert(ctx);
}

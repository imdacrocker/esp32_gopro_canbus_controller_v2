/*
 * cohn.c — COHN provisioning sequence and NVS credential storage.
 *
 * Sequence (matches the gopro-sdk-py and OpenGoPro tutorial flow):
 *   1. RequestStartScan       → puts camera into STA mode and triggers a scan.
 *                               Without this step, RequestConnectNew silently
 *                               fails because the camera stays in AP mode.
 *   2. RequestConnectNew      → camera joins our SoftAP. The camera also
 *                               auto-creates the COHN TLS cert here, so we
 *                               skip the explicit CreateCert + SetSetting
 *                               (per gopro-sdk-py:
 *                                "RequestConnectNew automatically creates
 *                                 COHN certificate").
 *   3. RequestGetCOHNStatus   → poll until status=PROVISIONED and
 *                               state=NetworkConnected.  Extract credentials.
 *   4. Save creds to NVS cam_N/gopro_cohn.
 *   5. camera_manager_set_camera_ready() → triggers open_gopro_http probe.
 *
 * Channel routing per OpenGoPro spec:
 *   - Scan + ConnectNew (feature 0x02)  → GP-0091 net_mgmt_cmd_write
 *   - GetCOHNStatus     (feature 0xF5)  → GP-0076 query_write
 *
 * Notifications consumed during the sequence:
 *   - Action 0x82 (ResponseStartScanning)   — initial scan ack
 *   - Action 0x0B (NotifStartScanning)      — wait for SCANNING_SUCCESS=5
 *   - Action 0x85 (ResponseConnectNew)      — initial connect ack
 *   - Action 0x0C (NotifProvisioningState)  — wait for SUCCESS_NEW_AP=5
 *   - Action 0xEF (ResponseGetCOHNStatus)   — status response
 *
 * Protobuf encoding is hand-rolled (no protobuf library dependency).
 * Field tag byte = (field_number << 3) | wire_type.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
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
    COHN_STEP_SCAN,          /* RequestStartScan, await NotifStartScanning(SUCCESS) */
    COHN_STEP_CONNECT,       /* RequestConnectNew, await NotifProvisioningState(SUCCESS_*_AP) */
    COHN_STEP_CREATE_CERT,   /* RequestCreateCOHNCert{override=true}, await ResponseCreateCert */
    COHN_STEP_GET_STATUS,    /* RequestGetCOHNStatus polling */
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
 * Build a GPBS-framed protobuf feature command:
 *   payload = [feature_id, action_id, pb_body...]
 * Returns the total packet length (GPBS header + payload).
 */
static int build_proto_pkt(uint8_t *pkt, uint8_t feature_id, uint8_t action_id,
                            const uint8_t *pb_body, uint8_t pb_len)
{
    uint8_t payload[128];
    int     plen = 0;
    payload[plen++] = feature_id;
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

static void send_scan(gopro_ble_ctx_t *ctx)
{
    uint8_t pkt[8];
    /* RequestStartScan: no protobuf body. */
    int len = build_proto_pkt(pkt, GOPRO_PROTO_FEATURE_NETWORK_MGMT,
                               GOPRO_NETMGMT_ACTION_SCAN, NULL, 0);
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.net_mgmt_cmd_write, pkt, len);
    ESP_LOGI(TAG, "slot %d: → RequestStartScan", ctx->slot);
}

static void send_create_cert(gopro_ble_ctx_t *ctx)
{
    /*
     * RequestCreateCOHNCert{override=true}.  Sent on the COMMAND channel
     * (GP-0072) after the camera has joined our SoftAP — this is what
     * actually transitions COHN status from UNPROVISIONED to PROVISIONED.
     * override=true forces a fresh cert even if the camera retained one
     * from a previous network.
     */
    uint8_t pb[4];
    int ppos = pb_write_varint_field(pb, 0,
                                      GOPRO_COHN_CREATE_PB_OVERRIDE_TAG, 1u);

    uint8_t pkt[16];
    int len = build_proto_pkt(pkt, GOPRO_PROTO_FEATURE_COMMAND,
                               GOPRO_COHN_ACTION_CREATE_CERT,
                               pb, (uint8_t)ppos);
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.cmd_write, pkt, len);
    ESP_LOGI(TAG, "slot %d: → RequestCreateCOHNCert{override}", ctx->slot);
}

static void send_connect_new(gopro_ble_ctx_t *ctx)
{
    /*
     * RequestConnectNew{ssid, password, bypass_eula_check=true}.
     * ssid + password are required (proto2); an open AP sends an empty pass.
     * bypass_eula_check skips the camera's "verify internet" gate — without
     * it, an isolated SoftAP causes the camera to stall waiting for an
     * internet uplink that doesn't exist.
     */
    wifi_config_t wc;
    char ssid[33] = {0};
    char pass[65] = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &wc) == ESP_OK) {
        snprintf(ssid, sizeof(ssid), "%s", (char *)wc.ap.ssid);
        snprintf(pass, sizeof(pass), "%s", (char *)wc.ap.password);
    }

    /*
     * Hero13 quirk: `password` is `required` in the RequestConnectNew proto,
     * so omitting it gives RESULT_ILL_FORMED (result=2).  An empty string
     * gives PROVISIONING_ERROR_PASSWORD_AUTH (state=8) — the camera treats
     * "" as a zero-length WPA-PSK key and fails the handshake against our
     * open AP.  Try a non-empty placeholder; if Hero13 keys auth selection
     * off the AP's advertised security (open in our case), the password is
     * ignored and the placeholder just satisfies the proto validator.
     */
    if (strlen(pass) == 0) {
        snprintf(pass, sizeof(pass), "00000000");
    }

    uint8_t pb[128];
    int ppos = 0;
    ppos = pb_write_string_field(pb, ppos, GOPRO_NETMGMT_PB_SSID_TAG,
                                  ssid, (uint8_t)strlen(ssid));
    ppos = pb_write_string_field(pb, ppos, GOPRO_NETMGMT_PB_PASS_TAG,
                                  pass, (uint8_t)strlen(pass));
    ppos = pb_write_varint_field(pb, ppos, GOPRO_NETMGMT_PB_BYPASS_EULA_TAG, 1u);

    uint8_t pkt[160];
    int len = build_proto_pkt(pkt, GOPRO_PROTO_FEATURE_NETWORK_MGMT,
                               GOPRO_NETMGMT_ACTION_CONNECT_NEW,
                               pb, (uint8_t)ppos);
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.net_mgmt_cmd_write, pkt, len);
    ESP_LOGI(TAG, "slot %d: → RequestConnectNew ssid=\"%s\"", ctx->slot, ssid);
}

/*
 * RequestGetCOHNStatus on the QUERY channel (feature 0xF5, GP-0076).
 * The first call sends register_cohn_status=true so the camera also pushes
 * status notifications spontaneously; later polls use an empty body.
 */
static void send_get_status(gopro_ble_ctx_t *ctx, bool register_for_pushes)
{
    uint8_t pb[4];
    int     ppos = 0;
    if (register_for_pushes) {
        ppos = pb_write_varint_field(pb, ppos,
                                      GOPRO_COHN_STATUS_PB_REGISTER_TAG, 1u);
    }

    uint8_t pkt[8];
    int len = build_proto_pkt(pkt, GOPRO_PROTO_FEATURE_QUERY,
                               GOPRO_COHN_ACTION_GET_STATUS,
                               ppos > 0 ? pb : NULL, (uint8_t)ppos);
    ble_core_gatt_write(ctx->conn_handle, ctx->gatt.query_write, pkt, len);
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
    send_get_status(ctx, /*register_for_pushes=*/false);
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

/*
 * Parse a ResponseGeneric / ResponseConnectNew body and return the value of
 * field 1 (EnumResultGeneric).  Returns 0 (UNKNOWN) if the field is missing.
 */
static uint8_t parse_generic_result(const uint8_t *body, uint16_t len)
{
    uint16_t pos = 0;
    while (pos < len) {
        uint8_t tag = pb_read_varint(body, len, &pos);
        if (tag == GOPRO_RESP_GENERIC_RESULT_TAG) {
            return pb_read_varint(body, len, &pos);
        }
        /* Skip unknown field. */
        uint8_t wire = tag & 0x07u;
        if (wire == 0) {
            pb_read_varint(body, len, &pos);
        } else if (wire == 2) {
            uint8_t skip = pb_read_varint(body, len, &pos);
            pos += skip;
        } else {
            return 0;
        }
    }
    return 0;
}

static void abort_provision(gopro_ble_ctx_t *ctx, cohn_state_t *cs,
                             const char *reason)
{
    ESP_LOGE(TAG, "slot %d: COHN provision aborted: %s", ctx->slot, reason);
    if (cs->poll_timer) {
        esp_timer_stop(cs->poll_timer);
    }
    cs->step = COHN_STEP_IDLE;
    ctx->cohn_provisioning = false;
}

void gopro_cohn_on_response(gopro_ble_ctx_t *ctx, uint8_t feature_id,
                             uint8_t action_id,
                             const uint8_t *body, uint16_t body_len)
{
    cohn_state_t *cs = &s_cohn[ctx->slot];

    /* If we are not in provisioning, ignore stray responses. */
    if (!ctx->cohn_provisioning) {
        ESP_LOGD(TAG, "slot %d: stray proto resp feat=0x%02x act=0x%02x",
                 ctx->slot, feature_id, action_id);
        return;
    }

    ESP_LOGD(TAG, "slot %d: cohn rx feat=0x%02x act=0x%02x body_len=%d step=%d",
             ctx->slot, feature_id, action_id, body_len, (int)cs->step);

    switch (action_id) {
    case GOPRO_NETMGMT_RESP_SCAN: {
        /* ResponseStartScanning — initial ack of the scan command. */
        if (cs->step != COHN_STEP_SCAN) break;
        uint8_t result = parse_generic_result(body, body_len);
        if (result != GOPRO_RESP_GENERIC_SUCCESS) {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "scan rejected (result=%u, camera may be in STA mode)",
                     result);
            abort_provision(ctx, cs, msg);
            return;
        }
        ESP_LOGD(TAG, "slot %d: scan started, awaiting completion notif",
                 ctx->slot);
        break;
    }

    case GOPRO_NETMGMT_NOTIF_SCAN: {
        /* NotifStartScanning — wait for SCANNING_SUCCESS. */
        if (cs->step != COHN_STEP_SCAN) break;
        uint8_t state = parse_generic_result(body, body_len);
        if (state == GOPRO_NETMGMT_SCAN_SUCCESS) {
            ESP_LOGI(TAG, "slot %d: scan complete, requesting AP connect",
                     ctx->slot);
            cs->step = COHN_STEP_CONNECT;
            send_connect_new(ctx);
        } else if (state == GOPRO_NETMGMT_SCAN_ABORTED_BY_SYSTEM ||
                   state == GOPRO_NETMGMT_SCAN_CANCELLED_BY_USER) {
            char msg[48];
            snprintf(msg, sizeof(msg), "scan aborted (state=%u)", state);
            abort_provision(ctx, cs, msg);
        } else {
            ESP_LOGD(TAG, "slot %d: scan progress state=%u",
                     ctx->slot, state);
        }
        break;
    }

    case GOPRO_NETMGMT_RESP_CONNECT_NEW: {
        /* ResponseConnectNew — initial ack; SUCCESS means accepted, not
         * finished. The actual connect completion comes via NotifProvis. */
        if (cs->step != COHN_STEP_CONNECT) break;
        uint8_t result = parse_generic_result(body, body_len);
        if (result != GOPRO_RESP_GENERIC_SUCCESS) {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "ConnectNew rejected (EnumResultGeneric=%u)", result);
            abort_provision(ctx, cs, msg);
            return;
        }
        ESP_LOGI(TAG, "slot %d: AP connect accepted, awaiting provisioning",
                 ctx->slot);
        break;
    }

    case GOPRO_NETMGMT_NOTIF_PROVIS: {
        /* NotifProvisioningState — wait for SUCCESS_*_AP. */
        if (cs->step != COHN_STEP_CONNECT) break;
        uint8_t state = parse_generic_result(body, body_len);
        if (state == GOPRO_NETMGMT_PROVIS_SUCCESS_NEW_AP ||
            state == GOPRO_NETMGMT_PROVIS_SUCCESS_OLD_AP) {
            ESP_LOGI(TAG, "slot %d: WiFi connected (EnumProvisioning=%u), creating COHN cert",
                     ctx->slot, state);
            cs->step = COHN_STEP_CREATE_CERT;
            send_create_cert(ctx);
        } else if (state == GOPRO_NETMGMT_PROVIS_STARTED) {
            ESP_LOGD(TAG, "slot %d: provisioning in progress", ctx->slot);
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "provisioning failed (EnumProvisioning=%u)", state);
            abort_provision(ctx, cs, msg);
        }
        break;
    }

    case GOPRO_COHN_RESP_CREATE_CERT: {
        /* ResponseCreateCOHNCert (ResponseGeneric body). On SUCCESS the
         * camera begins generating the cert and transitions COHN status
         * toward PROVISIONED — start polling. */
        if (cs->step != COHN_STEP_CREATE_CERT) break;
        uint8_t result = parse_generic_result(body, body_len);
        if (result != GOPRO_RESP_GENERIC_SUCCESS) {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "CreateCert rejected (EnumResultGeneric=%u)", result);
            abort_provision(ctx, cs, msg);
            return;
        }
        ESP_LOGI(TAG, "slot %d: CreateCert accepted, polling COHN status",
                 ctx->slot);
        cs->step       = COHN_STEP_GET_STATUS;
        cs->poll_count = 0;
        send_get_status(ctx, /*register_for_pushes=*/true);
        esp_timer_start_periodic(cs->poll_timer,
                                  COHN_STATUS_POLL_INTERVAL_MS * 1000ULL);
        break;
    }

    case GOPRO_COHN_RESP_GET_STATUS: {
        /*
         * Body is a NotifyCOHNStatus protobuf.  We extract everything the
         * camera_manager needs in one pass: the readiness fields (status,
         * state, enabled), the credentials (username, password), and the
         * routing metadata (ipaddress, macaddress).  The MAC and IP are
         * required because the camera's WiFi MAC differs from its BLE peer
         * MAC, so the slot wouldn't otherwise be findable from DHCP events.
         */
        uint16_t pos = 0;
        uint8_t  status      = 0;
        uint8_t  net_state   = 0;
        bool     enabled     = false;
        char     user[32]    = {0};
        char     pass[64]    = {0};
        char     ip_str[20]  = {0};
        char     mac_str[16] = {0};

        while (pos < body_len) {
            uint8_t tag = pb_read_varint(body, body_len, &pos);
            switch (tag) {
            case GOPRO_COHN_PB_STATUS_TAG:
                status = pb_read_varint(body, body_len, &pos);
                break;
            case GOPRO_COHN_PB_STATE_TAG:
                net_state = pb_read_varint(body, body_len, &pos);
                break;
            case GOPRO_COHN_PB_USER_TAG:
                pb_read_string(body, body_len, &pos, user, sizeof(user));
                break;
            case GOPRO_COHN_PB_PASS_TAG:
                pb_read_string(body, body_len, &pos, pass, sizeof(pass));
                break;
            case GOPRO_COHN_PB_IP_TAG:
                pb_read_string(body, body_len, &pos, ip_str, sizeof(ip_str));
                break;
            case GOPRO_COHN_PB_ENABLED_TAG:
                enabled = (pb_read_varint(body, body_len, &pos) != 0);
                break;
            case GOPRO_COHN_PB_MAC_TAG:
                pb_read_string(body, body_len, &pos, mac_str, sizeof(mac_str));
                break;
            default: {
                /* Skip unknown field. */
                uint8_t wire = tag & 0x07u;
                if (wire == 0) {
                    pb_read_varint(body, body_len, &pos);
                } else if (wire == 2) {
                    uint8_t skip = pb_read_varint(body, body_len, &pos);
                    pos += skip;
                } else {
                    pos = body_len;
                }
                break;
            }
            }
        }

        bool ready = enabled
                   && status   == GOPRO_COHN_STATUS_PROVISIONED
                   && net_state == GOPRO_COHN_STATE_NET_CONNECTED
                   && user[0]    != '\0'
                   && pass[0]    != '\0'
                   && ip_str[0]  != '\0'
                   && mac_str[0] != '\0';

        if (!ready) {
            ESP_LOGD(TAG,
                     "slot %d: COHN status (en=%d prov=%u state=%u "
                     "user=%d pass=%d ip=%d mac=%d)",
                     ctx->slot, enabled, status, net_state,
                     user[0] != '\0', pass[0] != '\0',
                     ip_str[0] != '\0', mac_str[0] != '\0');
            break;
        }

        /* Convert macaddress (12 hex chars, no separators) to 6 bytes. */
        uint8_t wifi_mac[6] = {0};
        bool mac_ok = (strlen(mac_str) == 12);
        for (int i = 0; mac_ok && i < 6; i++) {
            unsigned int b = 0;
            if (sscanf(mac_str + 2 * i, "%2x", &b) != 1) {
                mac_ok = false;
                break;
            }
            wifi_mac[i] = (uint8_t)b;
        }

        /* Convert ipaddress dotted-decimal to lwip's u32 layout. */
        ip4_addr_t addr = {0};
        bool ip_ok = (ip4addr_aton(ip_str, &addr) == 1);

        if (!mac_ok || !ip_ok) {
            ESP_LOGW(TAG, "slot %d: COHN status had bad mac=\"%s\" or ip=\"%s\"",
                     ctx->slot, mac_str, ip_str);
            break;
        }

        esp_timer_stop(cs->poll_timer);
        cs->step = COHN_STEP_IDLE;
        ctx->cohn_provisioning = false;

        ESP_LOGI(TAG, "slot %d: COHN connected, saving credentials", ctx->slot);
        extern bool gopro_cohn_save(int slot, const char *user, const char *pass);
        gopro_cohn_save(ctx->slot, user, pass);
        camera_manager_on_cohn_provisioned(ctx->slot, wifi_mac, addr.addr);
        break;
    }

    default:
        ESP_LOGW(TAG, "slot %d: unknown proto response feat=0x%02x act=0x%02x",
                 ctx->slot, feature_id, action_id);
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
    if (ctx->conn_handle == GOPRO_CONN_NONE ||
        ctx->gatt.cmd_write == 0 ||
        ctx->gatt.net_mgmt_cmd_write == 0 ||
        ctx->gatt.query_write == 0) {
        ESP_LOGE(TAG, "slot %d: cannot provision — not connected or missing GATT handles",
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
    cs->step               = COHN_STEP_SCAN;
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

    send_scan(ctx);
}

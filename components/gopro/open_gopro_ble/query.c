/*
 * query.c — GPBS packet reassembly and response dispatch.
 *
 * GoPro cameras may fragment long responses across multiple ATT notifications.
 * Each slot/channel pair has an independent reassembly buffer.  When a complete
 * response is accumulated, it is dispatched to the appropriate handler.
 *
 * Reassembly header formats:
 *
 *   Start packet:
 *     General  (len ≤ 31):  byte[0] = 0b000LLLLL
 *     Ext-13   (len ≤ 8191): byte[0] = 0b001HHHHH, byte[1] = LLLLLLLL
 *     Ext-16   (len ≤ 65535): byte[0] = 0b010?????, byte[1-2] = length
 *
 *   Continuation packet:
 *     byte[0] = 0b1SSSSSSS  (S = 7-bit sequence number, counted from 0)
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/protocol/data_protocol.html
 */

#include <string.h>
#include "esp_log.h"
#include "open_gopro_ble_internal.h"

static const char *TAG = "gopro_ble/query";

/* ---- Per-slot/channel reassembly state ----------------------------------- */

typedef struct {
    uint8_t  buf[GPBS_MAX_RESPONSE_LEN];
    uint16_t expected_len;
    uint16_t received;
    bool     active;
    uint8_t  next_seq;
} reassembly_t;

/* 4 channels per slot */
static reassembly_t s_asm[CAMERA_MAX_SLOTS][4];

static reassembly_t *get_asm(gopro_ble_ctx_t *ctx, gopro_channel_t chan)
{
    if (ctx->slot < 0 || ctx->slot >= CAMERA_MAX_SLOTS) {
        return NULL;
    }
    return &s_asm[ctx->slot][(int)chan];
}

/* ---- Response dispatch --------------------------------------------------- */

/* Forward declarations from readiness.c and cohn.c */
extern void gopro_readiness_on_response(gopro_ble_ctx_t *ctx,
                                         const uint8_t *data, uint16_t len);
extern void gopro_cohn_on_response(gopro_ble_ctx_t *ctx, uint8_t feature_id,
                                    uint8_t action_id,
                                    const uint8_t *body, uint16_t body_len);

/*
 * A protobuf feature response on any channel: [feature_id, action_id, body...].
 * Strip the 2-byte header and dispatch the body to the COHN handler.
 */
static void dispatch_proto_resp(gopro_ble_ctx_t *ctx,
                                 const uint8_t *data, uint16_t len)
{
    if (len < GOPRO_PROTO_RESP_HDR_LEN) {
        ESP_LOGW(TAG, "slot %d: short proto resp len=%d", ctx->slot, len);
        return;
    }
    uint8_t feature_id = data[GOPRO_PROTO_RESP_FEATURE_IDX];
    uint8_t action_id  = data[GOPRO_PROTO_RESP_ACTION_IDX];
    ESP_LOGD(TAG, "slot %d: proto resp feat=0x%02x act=0x%02x body_len=%d",
             ctx->slot, feature_id, action_id,
             len - GOPRO_PROTO_RESP_HDR_LEN);
    gopro_cohn_on_response(ctx, feature_id, action_id,
                           data + GOPRO_PROTO_RESP_HDR_LEN,
                           len - GOPRO_PROTO_RESP_HDR_LEN);
}

static void dispatch(gopro_ble_ctx_t *ctx, gopro_channel_t chan,
                     const uint8_t *data, uint16_t len)
{
    if (len == 0) {
        return;
    }

    /*
     * Protobuf feature responses can arrive on multiple notify pipes:
     *   - GP-0073 (cmd channel)     for feature 0xF1 (COMMAND)
     *   - GP-0077 (query channel)   for feature 0xF5 (QUERY)
     *   - GP-0092 (net_mgmt channel) for feature 0x02 (NETWORK_MGMT)
     * Detect by the feature_id at byte 0 — all known feature IDs lie outside
     * the value space used by TLV command IDs, so the same byte unambiguously
     * tags the format.
     */
    uint8_t b0 = data[0];
    if (b0 == GOPRO_PROTO_FEATURE_NETWORK_MGMT ||
        b0 == GOPRO_PROTO_FEATURE_COMMAND ||
        b0 == GOPRO_PROTO_FEATURE_QUERY) {
        dispatch_proto_resp(ctx, data, len);
        return;
    }

    /* Otherwise, fall through to channel-specific TLV handling. */
    switch (chan) {
    case GOPRO_CHAN_CMD:
        if (len >= GOPRO_RESP_HDR_LEN &&
            data[GOPRO_RESP_CMD_IDX] == GOPRO_CMD_GET_HARDWARE_INFO) {
            gopro_readiness_on_response(ctx, data, len);
        } else if (len >= GOPRO_RESP_HDR_LEN) {
            ESP_LOGD(TAG, "slot %d: cmd resp cmd=0x%02x status=0x%02x",
                     ctx->slot,
                     data[GOPRO_RESP_CMD_IDX],
                     data[GOPRO_RESP_STATUS_IDX]);
        }
        break;

    case GOPRO_CHAN_SETTINGS:
        if (len >= GOPRO_RESP_HDR_LEN) {
            ESP_LOGD(TAG, "slot %d: settings resp cmd=0x%02x status=0x%02x",
                     ctx->slot,
                     data[GOPRO_RESP_CMD_IDX],
                     data[GOPRO_RESP_STATUS_IDX]);
        }
        break;

    case GOPRO_CHAN_QUERY:
        ESP_LOGD(TAG, "slot %d: query resp len=%d", ctx->slot, len);
        break;

    case GOPRO_CHAN_NET_MGMT:
        ESP_LOGW(TAG, "slot %d: unexpected non-protobuf net_mgmt resp byte0=0x%02x",
                 ctx->slot, b0);
        break;
    }
}

/* ---- Feed ---------------------------------------------------------------- */

void gopro_query_feed(gopro_ble_ctx_t *ctx, gopro_channel_t chan,
                      const uint8_t *data, uint16_t len)
{
    reassembly_t *a = get_asm(ctx, chan);
    if (!a || len == 0) {
        return;
    }

    bool is_continuation = (data[0] & GPBS_HDR_CONTINUATION) != 0;

    if (!is_continuation) {
        /* --- Start packet --- */
        uint8_t hdr_type = (data[0] >> 5) & 0x03u;
        uint16_t payload_len;
        uint16_t hdr_bytes;

        switch (hdr_type) {
        case 0: /* General: length in bits[4:0] */
            payload_len = data[0] & 0x1Fu;
            hdr_bytes   = 1;
            break;
        case 1: /* Extended 13-bit */
            if (len < 2) { return; }
            payload_len = ((uint16_t)(data[0] & 0x1Fu) << 8) | data[1];
            hdr_bytes   = 2;
            break;
        case 2: /* Extended 16-bit */
            if (len < 3) { return; }
            payload_len = ((uint16_t)data[1] << 8) | data[2];
            hdr_bytes   = 3;
            break;
        default:
            ESP_LOGW(TAG, "slot %d: reserved GPBS header type", ctx->slot);
            return;
        }

        if (payload_len > GPBS_MAX_RESPONSE_LEN) {
            ESP_LOGW(TAG, "slot %d: response too large (%d B), discarding",
                     ctx->slot, payload_len);
            return;
        }

        a->expected_len = payload_len;
        a->received     = 0;
        a->active       = true;
        a->next_seq     = 0;

        uint16_t body_in_pkt = len - hdr_bytes;
        if (body_in_pkt > payload_len) {
            body_in_pkt = payload_len;
        }
        memcpy(a->buf, data + hdr_bytes, body_in_pkt);
        a->received = body_in_pkt;

    } else {
        /* --- Continuation packet --- */
        if (!a->active) {
            ESP_LOGW(TAG, "slot %d chan=%d: continuation without start, discarding",
                     ctx->slot, (int)chan);
            return;
        }
        uint8_t seq = data[0] & 0x7Fu;
        if (seq != a->next_seq) {
            ESP_LOGW(TAG, "slot %d: seq mismatch exp=%d got=%d, resetting",
                     ctx->slot, a->next_seq, seq);
            a->active = false;
            return;
        }
        a->next_seq++;

        uint16_t body_in_pkt = len - 1;
        uint16_t remaining   = a->expected_len - a->received;
        if (body_in_pkt > remaining) {
            body_in_pkt = remaining;
        }
        memcpy(a->buf + a->received, data + 1, body_in_pkt);
        a->received += body_in_pkt;
    }

    if (a->received >= a->expected_len) {
        a->active = false;
        dispatch(ctx, chan, a->buf, a->expected_len);
    }
}

/* ---- Free ---------------------------------------------------------------- */

void gopro_query_free(gopro_ble_ctx_t *ctx)
{
    if (ctx->slot < 0 || ctx->slot >= CAMERA_MAX_SLOTS) {
        return;
    }
    memset(s_asm[ctx->slot], 0, sizeof(s_asm[ctx->slot]));
}

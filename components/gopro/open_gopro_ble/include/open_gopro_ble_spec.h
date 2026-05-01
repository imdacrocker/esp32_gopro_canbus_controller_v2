/*
 * open_gopro_ble_spec.h — Canonical BLE spec constants for Open GoPro.
 *
 * All raw byte values, packet templates, command IDs, response field offsets,
 * and GATT UUID definitions live here.  No other .c file embeds raw literals.
 *
 * Spec reference: https://gopro.github.io/OpenGoPro/ble
 */
#pragma once

#include <stdint.h>
#include "host/ble_hs.h"

/* ---- GoPro advertisement filter ----------------------------------------- */

/* 16-bit service UUID advertised by all Open GoPro cameras in scan response. */
#define GOPRO_SVC_UUID16  0xFEA6

/* ---- GoPro GATT UUID base ------------------------------------------------ */

/*
 * All GoPro characteristics share the 128-bit base UUID:
 *   b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b
 *
 * NimBLE stores 128-bit UUIDs in little-endian byte order.
 * Canonical bytes:  b5 f9 XX XX  aa 8d  11 e3  90 46  00 02 a5 d5 c5 1b
 * LE (reversed):    1b c5 d5 a5  02 00  46 90  e3 11  8d aa  LO HI  f9 b5
 *
 * The XX XX suffix occupies bytes [12] (lo) and [13] (hi) of the LE array.
 * All currently used suffixes have a 0x00 high byte (range 0x0001–0x0092).
 */
#define GOPRO_UUID128_INIT(suf_lo) \
    BLE_UUID128_INIT(0x1b, 0xc5, 0xd5, 0xa5, 0x02, 0x00, 0x46, 0x90, \
                     0xe3, 0x11, 0x8d, 0xaa, (suf_lo), 0x00, 0xf9, 0xb5)

/* ---- GATT characteristic UUID suffixes ----------------------------------- */

/* Command channel */
#define GOPRO_CHR_CMD_WRITE_UUID           GOPRO_UUID128_INIT(0x72)  /* GP-0072 Write */
#define GOPRO_CHR_CMD_RESP_NOTIFY_UUID     GOPRO_UUID128_INIT(0x73)  /* GP-0073 Notify */

/* Settings channel */
#define GOPRO_CHR_SETTINGS_WRITE_UUID      GOPRO_UUID128_INIT(0x74)  /* GP-0074 Write */
#define GOPRO_CHR_SETTINGS_RESP_UUID       GOPRO_UUID128_INIT(0x75)  /* GP-0075 Notify */

/* Query channel */
#define GOPRO_CHR_QUERY_WRITE_UUID         GOPRO_UUID128_INIT(0x76)  /* GP-0076 Write */
#define GOPRO_CHR_QUERY_RESP_NOTIFY_UUID   GOPRO_UUID128_INIT(0x77)  /* GP-0077 Notify */

/* Network management channel (COHN provisioning) */
#define GOPRO_CHR_NET_MGMT_CMD_UUID        GOPRO_UUID128_INIT(0x91)  /* GP-0091 Write */
#define GOPRO_CHR_NET_MGMT_RESP_UUID       GOPRO_UUID128_INIT(0x92)  /* GP-0092 Notify */

/* WiFi AP control (used by gopro_wifi_rc for RC-emulation cameras) */
#define GOPRO_CHR_WIFI_AP_PWR_UUID         GOPRO_UUID128_INIT(0x01)  /* GP-0001 Write */
#define GOPRO_CHR_WIFI_AP_SSID_UUID        GOPRO_UUID128_INIT(0x02)  /* GP-0002 Read */
#define GOPRO_CHR_WIFI_AP_PASS_UUID        GOPRO_UUID128_INIT(0x03)  /* GP-0003 Read */
#define GOPRO_CHR_WIFI_AP_STATE_UUID       GOPRO_UUID128_INIT(0x04)  /* GP-0004 Indicate */

/* CCCD descriptor UUID (standard BLE) */
#define BLE_GATT_DSC_CLT_CFG_UUID16  0x2902

/* ---- GPBS packet encoding ------------------------------------------------ */

/*
 * GoPro BLE Specification (GPBS) header byte layout:
 *
 *   Bit 7    : 0 = start packet, 1 = continuation packet
 *   Bits 6-5 : (start only) 00 = general (≤31 B), 01 = ext-13, 10 = ext-16
 *   Bits 4-0 : (start, general) payload length
 *   Bits 6-0 : (continuation) sequence number
 */
#define GPBS_HDR_CONTINUATION  0x80u
#define GPBS_HDR_EXT13         0x20u
#define GPBS_HDR_EXT16         0x40u
#define GPBS_HDR_GENERAL_MAX   31u   /* max payload length for general header */

/* Maximum reassembled response buffer (longest GoPro response observed ~256 B) */
#define GPBS_MAX_RESPONSE_LEN  512u

/* ---- CCCD subscription values -------------------------------------------- */
#define BLE_CCCD_NOTIFY   0x0001u
#define BLE_CCCD_INDICATE 0x0002u

/* ---- TLV command IDs ----------------------------------------------------- */

/*
 * GetHardwareInfo (0x3C)
 * Written to: cmd_write (GP-0072)
 * Response on: cmd_resp_notify (GP-0073)
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/query.html#get-hardware-info
 */
#define GOPRO_CMD_GET_HARDWARE_INFO  0x3Cu

/*
 * SetDateTime (0x0D)
 * Written to: cmd_write (GP-0072)
 * Response on: cmd_resp_notify (GP-0073)
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/control.html#set-date-time
 *
 * Payload TLV:
 *   [0x01, 0x04, year_hi, year_lo, month, day]   (date param)
 *   [0x02, 0x03, hour, minute, second]            (time param)
 */
#define GOPRO_CMD_SET_DATE_TIME  0x0Du

#define GOPRO_DT_PARAM_DATE  0x01u
#define GOPRO_DT_PARAM_DATE_LEN  4u   /* year(2B big-endian), month, day */
#define GOPRO_DT_PARAM_TIME  0x02u
#define GOPRO_DT_PARAM_TIME_LEN  3u   /* hour, minute, second */

/* Full SetDateTime packet: header + cmd + date_TLV + time_TLV */
#define GOPRO_SET_DATETIME_PAYLOAD_LEN \
    (1u + 2u + GOPRO_DT_PARAM_DATE_LEN + 2u + GOPRO_DT_PARAM_TIME_LEN)  /* 14 bytes */

/*
 * BLE keepalive
 * Written to: settings_write (GP-0074)
 * Payload: [0x01, 0x42] — length=1, value=0x42
 * Period: 3 seconds
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/control.html#keep-alive
 */
#define GOPRO_CMD_KEEPALIVE           0x42u
#define GOPRO_KEEPALIVE_PERIOD_MS     3000u

static const uint8_t k_gopro_keepalive_pkt[2] = { 0x01u, GOPRO_CMD_KEEPALIVE };

/* ---- TLV command response format ----------------------------------------- */

/*
 * Command response layout (reassembled GPBS payload):
 *   [0]: response marker (0x02 = command response)
 *   [1]: command ID (echoes the request cmd ID)
 *   [2]: result code (0x00 = success)
 *   [3..]: TLV parameter data (command-specific)
 */
#define GOPRO_RESP_HDR_LEN     3u
#define GOPRO_RESP_MARKER      0x02u
#define GOPRO_RESP_CMD_IDX     1u
#define GOPRO_RESP_STATUS_IDX  2u
#define GOPRO_RESP_STATUS_OK   0x00u

/* ---- GetHardwareInfo response TLV parameter IDs -------------------------- */

/*
 * TLV parameters in GetHardwareInfo response body (after 3-byte response header):
 *   0x01: manufacturer name (string)
 *   0x02: model number (uint32_t, big-endian) ← maps to camera_model_t
 *   0x03: model name (string)
 *   0x04: board type (string)
 *   0x05: firmware version (string)
 *   0x06: serial number (string)
 *   0x07: AP SSID (string)
 *   0x08: AP MAC address (6 bytes)
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/query.html#get-hardware-info
 */
#define GOPRO_HWINFO_PARAM_MODEL_NUM  0x02u

/* ---- Network management (COHN) protobuf encoding ------------------------- */

/*
 * COHN provisioning commands use the GPBS protobuf channel:
 *
 * Packet payload (after GPBS length header):
 *   [0]: 0xF1 — protobuf feature command marker
 *   [1]: feature ID (0x02 = Network Management)
 *   [2]: action ID (see below)
 *   [3..]: protobuf-encoded request body
 *
 * Response payload (from net_mgmt_resp_notify GP-0092):
 *   [0]: 0xF5 — protobuf feature response marker
 *   [1]: feature ID
 *   [2]: action ID (response version, high bit set)
 *   [3]: result code (0x00 = success)
 *   [4..]: protobuf-encoded response body
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/network_management.html
 */
#define GOPRO_PROTO_FEATURE_NET_MGMT   0x02u
#define GOPRO_PROTO_CMD_MARKER         0xF1u
#define GOPRO_PROTO_RESP_MARKER        0xF5u

/* Action IDs for Network Management feature */
#define GOPRO_COHN_ACTION_CREATE_CERT  0xF4u
#define GOPRO_COHN_ACTION_SET_AP       0xF0u
#define GOPRO_COHN_ACTION_CONNECT      0xF2u
#define GOPRO_COHN_ACTION_GET_STATUS   0xF8u

/* COHN response action IDs (response bit pattern) */
#define GOPRO_COHN_RESP_CREATE_CERT    0xF5u
#define GOPRO_COHN_RESP_SET_AP         0xF1u
#define GOPRO_COHN_RESP_CONNECT        0xF3u
#define GOPRO_COHN_RESP_GET_STATUS     0xF9u

/* COHN status result code (success) */
#define GOPRO_COHN_RESULT_OK           0x00u

/*
 * COHN protobuf field numbers for RequestGetCOHNStatus response:
 *   field 1 (varint): status enum (3 = CONNECTED)
 *   field 2 (string): username
 *   field 3 (string): password
 *   field 4 (string): ipaddress (camera's IP on the AP)
 *
 * Protobuf wire types: 0=varint, 1=64-bit, 2=len-delim, 5=32-bit
 * Field tag byte = (field_number << 3) | wire_type
 */
#define GOPRO_COHN_PB_STATUS_TAG     0x08u  /* field 1, varint */
#define GOPRO_COHN_PB_USER_TAG       0x12u  /* field 2, len-delim */
#define GOPRO_COHN_PB_PASS_TAG       0x1Au  /* field 3, len-delim */
#define GOPRO_COHN_STATUS_CONNECTED  3u

/*
 * Protobuf field numbers for RequestSetApEntries:
 *   field 1 (string): ssid
 *   field 2 (string): password  (empty for open AP)
 *   field 3 (varint): security_type (0 = open)
 */
#define GOPRO_SETAP_PB_SSID_TAG       0x0Au  /* field 1, len-delim */
#define GOPRO_SETAP_PB_PASS_TAG       0x12u  /* field 2, len-delim */
#define GOPRO_SETAP_PB_SECTYPE_TAG    0x18u  /* field 3, varint */
#define GOPRO_SETAP_SECURITY_OPEN     0x00u

/* ---- COHN net_mgmt response header indices ------------------------------ */

#define GOPRO_NMGMT_RESP_MARKER_IDX   0u
#define GOPRO_NMGMT_RESP_FEATURE_IDX  1u
#define GOPRO_NMGMT_RESP_ACTION_IDX   2u
#define GOPRO_NMGMT_RESP_RESULT_IDX   3u
#define GOPRO_NMGMT_RESP_HDR_LEN      4u

/* ---- Readiness poll parameters ------------------------------------------- */

#define GOPRO_READINESS_RETRY_MAX     10u
#define GOPRO_READINESS_RETRY_MS      3000u

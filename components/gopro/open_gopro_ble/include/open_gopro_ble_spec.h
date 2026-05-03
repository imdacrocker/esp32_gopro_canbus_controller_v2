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

#define GOPRO_CHR_WIFI_AP_SSID_UUID        GOPRO_UUID128_INIT(0x02)  /* GP-0002 Read */
#define GOPRO_CHR_WIFI_AP_PASS_UUID        GOPRO_UUID128_INIT(0x03)  /* GP-0003 Read */
#define GOPRO_CHR_WIFI_AP_PWR_UUID         GOPRO_UUID128_INIT(0x04)  /* GP-0004 Write */
#define GOPRO_CHR_WIFI_AP_STATE_UUID       GOPRO_UUID128_INIT(0x05)  /* GP-0005 Read/Indicate */

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
 *   [0]: command/setting ID (echoes the request)
 *   [1]: command status (0x00 = success)
 *   [2..]: optional response data (command-specific TLV parameters)
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/protocol/data_protocol.html#command-response
 */
#define GOPRO_RESP_HDR_LEN     2u
#define GOPRO_RESP_CMD_IDX     0u
#define GOPRO_RESP_STATUS_IDX  1u
#define GOPRO_RESP_STATUS_OK   0x00u

/* ---- GetHardwareInfo response body layout -------------------------------- */

/*
 * The body is a sequence of positional length-value fields (NOT TLV).
 * Each field is [len (1B), value (len B)], in this fixed order:
 *
 *   1) model number   (uint32_t, big-endian) ← maps to camera_model_t
 *   2) model name     (string)
 *   3) deprecated     (string, ignored)
 *   4) firmware       (string)
 *   5) serial number  (string)
 *   6) AP SSID        (string)
 *   7) AP MAC address (6 raw bytes)
 *
 * Parsing lives in readiness.c (parse_and_log_hw_info).
 *
 * Spec: https://gopro.github.io/OpenGoPro/ble/features/query.html#get-hardware-info
 */

/* ---- Multi-channel protobuf operations ----------------------------------- */

/*
 * Some BLE features are exposed as protobuf-encoded action commands rather
 * than TLV.  Each request and response is wrapped with a 2-byte header
 * identifying the feature and action; the rest of the payload is a protobuf
 * message defined in the OpenGoPro .proto files.
 *
 * Packet payload (after the GPBS length header):
 *   [0]: feature_id   — see GOPRO_PROTO_FEATURE_*
 *   [1]: action_id    — request: 0x05, 0x65, 0x67, 0x6F, ...
 *                       response: request_action | 0x80
 *   [2..]: protobuf body
 *
 * Routing:
 *   - Network-management actions (feature 0x02) are written to GP-0091 and
 *     responses arrive on GP-0092.
 *   - Command actions (feature 0xF1, includes COHN) are written to GP-0072
 *     and responses arrive on GP-0073, sharing the channel with TLV cmd
 *     responses (distinguished by data[0]: TLV cmd IDs are well below 0xF1).
 *
 * Result codes for ResponseGeneric / ResponseConnectNew come from the
 * EnumResultGeneric enum (1 = SUCCESS); the result is the first protobuf
 * field, NOT a fixed byte offset.
 *
 * Spec: OpenGoPro protobuf definitions at github.com/gopro/OpenGoPro/protobuf
 */
#define GOPRO_PROTO_FEATURE_NETWORK_MGMT  0x02u  /* WiFi connect, scan, AP entries */
#define GOPRO_PROTO_FEATURE_COMMAND       0xF1u  /* COHN cert/setting, camera control */
#define GOPRO_PROTO_FEATURE_QUERY         0xF5u  /* COHN status/cert reads, queries */

/* Network-management action IDs (feature 0x02) */
#define GOPRO_NETMGMT_ACTION_SCAN         0x02u  /* RequestStartScan */
#define GOPRO_NETMGMT_ACTION_CONNECT_NEW  0x05u  /* RequestConnectNew */
#define GOPRO_NETMGMT_RESP_SCAN           0x82u  /* ResponseStartScanning */
#define GOPRO_NETMGMT_RESP_CONNECT_NEW    0x85u  /* ResponseConnectNew */
#define GOPRO_NETMGMT_NOTIF_SCAN          0x0Bu  /* NotifStartScanning */
#define GOPRO_NETMGMT_NOTIF_PROVIS        0x0Cu  /* NotifProvisioningState */

/* COMMAND-feature action IDs (feature 0xF1) — COHN cert + setting writes. */
#define GOPRO_COHN_ACTION_SET_SETTING     0x65u
#define GOPRO_COHN_ACTION_CREATE_CERT     0x67u
#define GOPRO_COHN_RESP_SET_SETTING       0xE5u
#define GOPRO_COHN_RESP_CREATE_CERT       0xE7u

/* QUERY-feature action IDs (feature 0xF5) — COHN status / cert reads. */
#define GOPRO_COHN_ACTION_GET_STATUS      0x6Fu  /* RequestGetCOHNStatus */
#define GOPRO_COHN_RESP_GET_STATUS        0xEFu  /* ResponseGetCOHNStatus */

/* Scan progress (NotifStartScanning.scanning_state, EnumScanning) */
#define GOPRO_NETMGMT_SCAN_SUCCESS              5u
#define GOPRO_NETMGMT_SCAN_ABORTED_BY_SYSTEM    3u
#define GOPRO_NETMGMT_SCAN_CANCELLED_BY_USER    4u

/* Connect progress (NotifProvisioningState.provisioning_state, EnumProvisioning) */
#define GOPRO_NETMGMT_PROVIS_STARTED            2u
#define GOPRO_NETMGMT_PROVIS_SUCCESS_NEW_AP     5u
#define GOPRO_NETMGMT_PROVIS_SUCCESS_OLD_AP     6u
/* values 3, 4, 7-11 are abort/error states */

/* ---- Protobuf encoding helpers ------------------------------------------ */

/* Protobuf wire types: 0=varint, 1=64-bit, 2=len-delim, 5=32-bit. */
/* Field tag byte = (field_number << 3) | wire_type. */

/* ResponseGeneric / ResponseConnectNew share field 1 = EnumResultGeneric. */
#define GOPRO_RESP_GENERIC_RESULT_TAG  0x08u  /* field 1, varint */
#define GOPRO_RESP_GENERIC_SUCCESS     0x01u  /* EnumResultGeneric.RESULT_SUCCESS */

/* RequestCreateCOHNCert protobuf body. override=true forces the camera to
 * regenerate the certificate and (re)provision COHN even if it already had
 * a cert from a previous session. */
#define GOPRO_COHN_CREATE_PB_OVERRIDE_TAG 0x08u  /* field 1, varint bool */

/* RequestSetCOHNSetting protobuf body. */
#define GOPRO_COHN_SETTING_PB_ACTIVE_TAG  0x08u  /* field 1, varint bool */

/* RequestGetCOHNStatus protobuf body — register_cohn_status=true subscribes
 * to push notifications when status changes (in addition to our own polling). */
#define GOPRO_COHN_STATUS_PB_REGISTER_TAG 0x08u  /* field 1, varint bool */

/*
 * NotifyCOHNStatus protobuf field tags (response body of RequestGetCOHNStatus).
 *   field 1 varint:     EnumCOHNStatus       (1 = COHN_PROVISIONED)
 *   field 2 varint:     EnumCOHNNetworkState (27 = NetworkConnected)
 *   field 3 string:     username
 *   field 4 string:     password
 *   field 5 string:     ipaddress
 *   field 6 varint bool: enabled
 *   field 7 string:     ssid
 *   field 8 string:     macaddress
 */
#define GOPRO_COHN_PB_STATUS_TAG     0x08u  /* field 1, varint */
#define GOPRO_COHN_PB_STATE_TAG      0x10u  /* field 2, varint */
#define GOPRO_COHN_PB_USER_TAG       0x1Au  /* field 3, len-delim */
#define GOPRO_COHN_PB_PASS_TAG       0x22u  /* field 4, len-delim */
#define GOPRO_COHN_PB_IP_TAG         0x2Au  /* field 5, len-delim */
#define GOPRO_COHN_PB_ENABLED_TAG    0x30u  /* field 6, varint bool */
#define GOPRO_COHN_PB_MAC_TAG        0x42u  /* field 8, len-delim — camera's WiFi-side MAC */

#define GOPRO_COHN_STATUS_PROVISIONED   1u
#define GOPRO_COHN_STATE_NET_CONNECTED  27u

/*
 * RequestConnectNew protobuf field tags.  ssid + password are required by
 * the proto definition; pass an empty string for an open AP.
 *
 * bypass_eula_check (field 10) skips the camera's "verify internet" gate —
 * required for connecting to an isolated SoftAP with no internet uplink,
 * otherwise the camera stalls on EULA verification and never responds.
 */
#define GOPRO_NETMGMT_PB_SSID_TAG         0x0Au  /* field 1, len-delim */
#define GOPRO_NETMGMT_PB_PASS_TAG         0x12u  /* field 2, len-delim */
#define GOPRO_NETMGMT_PB_BYPASS_EULA_TAG  0x50u  /* field 10, varint bool */

/* ---- Protobuf response header indices ----------------------------------- */

#define GOPRO_PROTO_RESP_FEATURE_IDX  0u
#define GOPRO_PROTO_RESP_ACTION_IDX   1u
#define GOPRO_PROTO_RESP_HDR_LEN      2u

/* ---- Readiness poll parameters ------------------------------------------- */

#define GOPRO_READINESS_RETRY_MAX     10u
#define GOPRO_READINESS_RETRY_MS      3000u

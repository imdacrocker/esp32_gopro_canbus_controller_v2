# open_gopro_ble

Handles everything GoPro-specific that requires BLE: camera discovery, pairing, COHN credential provisioning, and ongoing BLE keepalive. It is the only component that registers callbacks with `ble_core` and the only component that writes GoPro BLE characteristics.

Recording commands are **not** sent over BLE. Once COHN provisioning is complete, all recording control travels over HTTPS via `open_gopro_http`. BLE is held open as a re-provisioning fallback and for the 3-second keepalive required to maintain the BLE link supervision timer.

---

## Responsibilities

- **Discovery**: filter BLE advertisements by the GoPro service UUID (`0xFEA6`); maintain a 10-entry list for the web UI (`GET /api/cameras`).
- **Pairing**: register new cameras into `camera_manager` on first bond; reconnect known cameras from NVS bond store.
- **GATT setup**: discover all characteristics across the full handle range, then subscribe CCCDs for all notify/indicate characteristics.
- **Readiness poll**: send `GetHardwareInfo` (0x3C) after GATT setup; retry up to 10× at 3-second intervals. Parse the positional LV body (model number, model name, firmware, serial, AP SSID, AP MAC), log them at INFO, and hand the model number to `camera_manager` as `camera_model_t`.
- **COHN provisioning**: when the camera is ready and no COHN credentials exist in NVS, run the 4-step provisioning sequence over the network management channel (GP-0091/0092).
- **UTC sync**: send `SetDateTime` to all connected cameras when UTC is live-synced this session; unblock any provisioning sequences that were waiting for it. `SetDateTime` is internally gated on `can_manager_utc_is_session_synced()`, so an NVS-restored UTC at boot does not push stale time to a camera.
- **BLE keepalive**: send `0x42` to the settings channel (GP-0074) every 3 seconds to prevent camera auto-sleep.
- **Re-provisioning**: expose `open_gopro_ble_reprovision()` for `open_gopro_http` to call after repeated HTTPS 401 responses.

---

## Dependencies

```
REQUIRES: bt, nvs_flash, esp_wifi, esp_timer, freertos, camera_manager, can_manager, ble_core
```

**Precondition:** `camera_manager_init()` must be called before `open_gopro_ble_init()`.  
`open_gopro_ble_init()` must be called before `ble_core_init()` — it registers the BLE callbacks that `ble_core` will use once the host task starts.

---

## Source Files

| File | Responsibility |
|------|---------------|
| `include/open_gopro_ble.h` | Public API |
| `include/open_gopro_ble_spec.h` | All raw constants: GoPro GATT UUIDs, command IDs, GPBS header format, COHN protobuf field tags, response field offsets |
| `open_gopro_ble_internal.h` | Private shared types (`gopro_gatt_handles_t`, `gopro_ble_ctx_t`, `gopro_channel_t`) and internal function declarations |
| `driver.c` | Per-slot context table, discovery list with UUID filter, `open_gopro_ble_init()` |
| `pairing.c` | `on_connected` / `on_encrypted` / `on_disconnected` callbacks registered with `ble_core` |
| `gatt.c` | Full-handle-range service/characteristic discovery, sequential CCCD subscription state machine. No explicit MTU exchange — the camera initiates and NimBLE replies with `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`. |
| `readiness.c` | `GetHardwareInfo` retry loop; positional length-value (LV) hardware-info parser that logs model number/name, firmware, serial, AP SSID and MAC; branches to provisioning or ready |
| `control.c` | `SetDateTime` TLV packet builder; 3-second periodic keepalive timer |
| `cohn.c` | COHN 4-step provisioning sequence; hand-rolled protobuf encode/decode; NVS `cam_N/gopro_cohn` read/write |
| `query.c` | GPBS packet reassembler (general / ext-13 / ext-16 headers, continuation packets); 4-channel response dispatch |
| `notify.c` | `on_notify_rx` callback — maps `attr_handle` to `gopro_channel_t`, feeds `gopro_query_feed()` |

---

## Public API

Header: `include/open_gopro_ble.h`

```c
/* Lifecycle */
void open_gopro_ble_init(void);

/* Discovery */
void open_gopro_ble_start_discovery(void);
void open_gopro_ble_stop_discovery(void);
int  open_gopro_ble_get_discovered(gopro_device_t *out, int max_count);

/* Connection */
void open_gopro_ble_connect_by_addr(const ble_addr_t *addr);

/* COHN credentials — called by open_gopro_http to build Basic Auth headers */
bool open_gopro_ble_get_cohn_credentials(int slot,
                                          char *user_out, size_t user_len,
                                          char *pass_out, size_t pass_len);

/* Re-provisioning — called by open_gopro_http after repeated HTTPS 401 */
void open_gopro_ble_reprovision(int slot);

/* UTC sync — called by main.c on the first live UTC source this session
 * (GPS frame or web-UI manual set).  Sends SetDateTime to every connected
 * slot and unblocks any cohn_pending_utc.  No-op for individual slots when
 * UTC is not session-synced (so an NVS-restored boot value cannot push
 * stale time to a camera). */
void open_gopro_ble_sync_time_all(void);
```

---

## GATT Channel Map

All GoPro characteristics use the 128-bit base UUID `b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b`.

| Handle name | UUID suffix | Direction | Channel |
|------------|-------------|-----------|---------|
| `cmd_write` | `0072` | Write | Commands (TLV) |
| `cmd_resp_notify` | `0073` | Notify | Command responses |
| `settings_write` | `0074` | Write | Settings / keepalive |
| `settings_resp_notify` | `0075` | Notify | Settings responses |
| `query_write` | `0076` | Write | Queries |
| `query_resp_notify` | `0077` | Notify | Query responses |
| `net_mgmt_cmd_write` | `0091` | Write | COHN provisioning (protobuf) |
| `net_mgmt_resp_notify` | `0092` | Notify | COHN responses |
| `wifi_ap_pwr_write` | `0001` | Write | WiFi AP power (RC-emulation) |
| `wifi_ap_ssid_read` | `0002` | Read | WiFi AP SSID (RC-emulation) |
| `wifi_ap_pass_read` | `0003` | Read | WiFi AP password (RC-emulation) |
| `wifi_ap_state_indicate` | `0004` | Indicate | WiFi AP state (RC-emulation) |

CCCDs are subscribed sequentially after characteristic discovery. GoPro cameras do not persist CCCD state across connections — subscriptions are re-sent on every reconnection.

---

## Connection State Machine

```
on_connected(conn_handle, addr)
  known camera  → record conn_handle, camera_manager_on_ble_connected()
  unknown       → deferred until on_encrypted

on_encrypted(conn_handle, addr)
  known camera  → camera_manager_on_ble_connected()
  new camera    → camera_manager_register_new(); if slots full → disconnect
  → snapshot current ATT MTU via ble_att_mtu() (camera will typically
    initiate an MTU exchange shortly after; we don't initiate one ourselves)
  → gopro_gatt_start_discovery()

gatt discovery complete
  → subscribe CCCDs sequentially (5 total)
  → gopro_readiness_start()

GetHardwareInfo (up to 10 retries × 3 s)
  success → extract model_num → camera_manager_set_model()
           → check NVS for cam_N/gopro_cohn
             credentials present  → SetDateTime (best-effort; internally
                                                  skipped unless UTC is
                                                  session-synced this boot)
                                  → camera_manager_on_ble_ready()
                                  → start keepalive timer
             credentials absent   → SetDateTime (same gating as above)
                                  → any UTC anchor (incl. NVS-restored)?
                                      yes → gopro_cohn_provision()
                                      no  → set cohn_pending_utc, start keepalive
  failure → retry; after 10 failures → ble_gap_terminate()

on_disconnected
  → cancel readiness timer, stop keepalive timer
  → free reassembly state
  → clear gopro_gatt_handles_t and conn_handle
  → camera_manager_on_ble_disconnected_by_handle()
```

---

## COHN Provisioning Sequence

Runs when `cam_N/gopro_cohn` has no valid credentials, or when `open_gopro_ble_reprovision()` is called.

```
1. RequestCreateCOHNCert   → GP-0091 (camera generates self-signed TLS cert)
2. RequestSetApEntries     → GP-0091 (SSID = "HERO-RC-XXXXXX", open security)
3. Camera auto-connects to SoftAP; wifi_manager fires on_station_ip
4. RequestGetCOHNStatus    → GP-0091, polled every 2 s (up to 15 attempts)
     status == CONNECTED   → extract username + password from protobuf response
5. Save to NVS: cam_N/gopro_cohn { cohn_user[32], cohn_pass[64] }
6. camera_manager_set_camera_ready(slot, true)
     → open_gopro_http probes the camera → WIFI_CAM_READY
```

TLS certificate verification is skipped (`ssl.CERT_NONE`). The SoftAP is a private closed network; Basic Auth credentials provide adequate protection on that link.

---

## GPBS Packet Reassembly

GoPro cameras may fragment long responses across multiple ATT notifications. `query.c` maintains a per-slot, per-channel reassembly buffer.

**Start packet header:**

| Header type | Byte 0 | Extra bytes | Max payload |
|------------|--------|-------------|-------------|
| General | `0b000LLLLL` (L = length) | — | 31 B |
| Extended 13-bit | `0b001HHHHH` | 1 (low 8 bits) | 8191 B |
| Extended 16-bit | `0b010_____` | 2 (16-bit length) | 65535 B |

**Continuation packet:** bit 7 set; low 7 bits = sequence number (starts at 0).

Maximum reassembled response: 512 bytes (covers all known GoPro responses including `GetHardwareInfo` at ~88 bytes and `GetCOHNStatus` at ~120 bytes).

---

## NVS Layout

This component exclusively owns the `gopro_cohn` key in each slot namespace. `camera_manager` never reads or writes it.

```
cam_0/gopro_cohn  ← gopro_cohn_nv_record_t { cohn_user[32], cohn_pass[64] }
cam_1/gopro_cohn
cam_2/gopro_cohn
cam_3/gopro_cohn
```

The key is erased at the start of a re-provisioning sequence and re-written only after a confirmed `RequestGetCOHNStatus` response with `status == CONNECTED`.

---

## BLE Keepalive

The keepalive prevents camera auto-sleep and maintains the BLE link supervision timer independently of WiFi/HTTP activity. It is required even when COHN is active and all recording commands are going over HTTPS.

- **Write target:** `settings_write` (GP-0074)
- **Payload:** `[0x01, 0x42]` (GPBS general header len=1, value=0x42)
- **Period:** 3 seconds (periodic `esp_timer`)
- **Started:** after `GetHardwareInfo` succeeds, before COHN provisioning
- **Stopped:** on disconnect, in `on_disconnected` before context is cleared

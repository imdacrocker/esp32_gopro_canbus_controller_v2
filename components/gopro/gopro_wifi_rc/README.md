# gopro_wifi_rc

Implements `camera_driver_t` for GoPro cameras using the legacy "WiFi Smart Remote" UDP protocol — Hero3 / Hero3+ / Hero4 / Hero5 / Hero7 / Hero8 all accept it as a backwards-compatible control channel. All recurring traffic — keepalive, status poll, shutter — is short binary UDP datagrams between this device's SoftAP (src port 8383) and the camera (dst port 8484). HTTP is used **only** for two off-path operations: an identify probe at pair time, and the optional date/time set; both target the camera's DHCP-assigned STA IP, not the literal `10.5.5.9` from the public docs.

---

## Responsibilities

- **Station lifecycle**: track GoPro cameras as they associate, get a DHCP lease, and disconnect.
- **Wake-on-LAN**: broadcast magic packet bursts when an associated camera goes silent for > 10 s.
- **UDP keepalive**: send `_GPHD_:0:0:2:0.000000\n` to each camera every 3 s (fire-and-forget).
- **UDP status poll**: send binary `st` to each camera with a known IP every 5 s; parse the response for power + recording state.
- **UDP shutter**: send binary `SH` packets (param 0x02 / 0x00) to all ready cameras.
- **HTTP identify** (once at pair time): `GET /gp/gpControl` → log model_name / model_number / firmware_version; map model into `camera_model_t`. On TCP RST or any failure, mark the slot as `CAMERA_MODEL_GOPRO_HERO_LEGACY_RC` and stay on the UDP-only path.
- **HTTP date/time** (best-effort): URL-encoded hex bytes; gated on `gopro_model_supports_http_datetime()`. No-op on the legacy fallback.
- **Liveness watchdog**: trigger WoL retry if `last_response_tick` ages past 10 s.

---

## Dependencies

```
REQUIRES: camera_manager, can_manager, wifi_manager, esp_timer, freertos, lwip, esp_wifi
```

**Precondition:** `camera_manager_init()` must be called before `gopro_wifi_rc_init()`.
`gopro_wifi_rc_init()` must be called before `wifi_manager_set_callbacks()` and `wifi_manager_init()` — it sets up the queues and tasks that the station callbacks post into.

---

## Transport Protocol

| Channel | Direction | Local | Remote | Notes |
|---|---|---|---|---|
| All UDP commands & replies | bidirectional | 8383 | 8484 | Single bound socket; src port 8383 is what Hero3-era cameras expect from a WiFi Remote |
| Wake-on-LAN | TX broadcast | — | 9 | 102-byte magic packet (6 × 0xFF + MAC × 16); burst of 5 |
| HTTP identify + date/time | TX only | — | 80/TCP | HTTP/1.0; only on Hero4-class cameras that run a server on their STA interface |

### Wire format (binary opcodes)

```
Byte:   0  1  2  3  4  5  6  7   8     9    10    11   12    13...
       [ -- 8 bytes of zero -- ] [SEL] [ ctr_hi ctr_lo ] [opcode 2 chars] [params]
```

`SEL` = `0x00` GET, `0x01` SET. Bytes 9–10 are static per opcode (verified working without rolling counters).

### Opcodes used

| Opcode | Sel | Length | Purpose | Response |
|---|---|---|---|---|
| `_GPHD_:0:0:2:0.000000\n` | (ASCII) | 22 B | Keepalive | `_GPHD_:0:0:2:\x01` (14 B, first byte 0x5F) |
| `s t` | GET | 13 B | Status request | 20 B; b13=power, b14=mode, b15=record state |
| `S H` | SET | 14 B | Shutter (param 0x02 start / 0x00 stop) | ACK |

### Status response decode

| b13 | b15 | meaning |
|---|---|---|
| 1 | × | camera off / sleeping → `RECORDING_UNKNOWN` |
| 0 | 1 | recording → `RECORDING_ACTIVE` |
| 0 | 0 | idle → `RECORDING_IDLE` |

### HTTP identify response

`GET /gp/gpControl HTTP/1.0` → JSON body containing an `info` object:

```json
"info": {
    "model_number": 13,
    "model_name": "HERO4 Black",
    "firmware_version": "HD4.02.05.00.00",
    ...
}
```

Three fields are extracted by substring search (no cJSON dependency). `model_name` is mapped into `camera_model_t` via `gopro_model_from_name()`. All three are logged at INFO so unrecognised models can be added to the lookup table over time.

### HTTP date/time format

```
GET /gp/gpControl/command/setup/date_time?p=%YY%MM%DD%hh%mm%ss HTTP/1.0\r\n\r\n
```

Each `%XX` is URL-encoded hex of one binary byte: year mod 100, month, day, hour, minute, second. Times are local; tz offset from `can_manager` should be applied before encoding (TODO).

---

## Public API

Header: `include/gopro_wifi_rc.h`

```c
/* Lifecycle */
void gopro_wifi_rc_init(void);

/* Station callbacks — wired by main.c via wifi_manager_set_callbacks() */
void gopro_wifi_rc_on_station_associated(const uint8_t mac[6]);
void gopro_wifi_rc_on_station_dhcp(const uint8_t mac[6], uint32_t ip);
void gopro_wifi_rc_on_station_disassociated(const uint8_t mac[6]);

/* Manual add/remove — called from http_server (POST /api/rc/add) */
void gopro_wifi_rc_add_camera(const uint8_t mac[6], uint32_t ip);
void gopro_wifi_rc_remove_camera(int slot);

/* Predicates — used by http_server for /api/rc/discovered */
bool gopro_wifi_rc_is_managed_slot(int slot);
bool gopro_wifi_rc_is_managed_mac(const uint8_t mac[6]);

/* UTC sync — called from main.c on_utc_acquired (first live UTC source this
 * session: CAN GPS frame or web-UI manual set).  Per-slot rc_send_datetime()
 * is internally a no-op until can_manager reports session-synced UTC AND
 * gopro_model_supports_http_datetime() returns true for the slot's model. */
void gopro_wifi_rc_sync_time_all(void);
```

All station callbacks and `sync_time_all` post to `s_work_queue` and return immediately — safe to call from the WiFi event task.

---

## Connection Flow

```
on_station_associated(mac)
  known RC slot, last_ip != 0  → send WoL burst, arm keepalive timer
  known RC slot, last_ip == 0  → wait for DHCP

on_station_dhcp(mac, ip)
  known RC slot → store last_ip, arm keepalive timer
                  (no probe — just wait for first response)

UDP RX (any opcode echoes from slot's IP, slot not yet ready)
  → set wifi_ready = true
  → camera_manager_on_camera_ready(slot)
  → post CMD_HTTP_IDENTIFY (once per session)
  → send date/time (gated on supports_http_datetime)

UDP RX (st response)
  → ctx->recording_status from b13/b15

CMD_HTTP_IDENTIFY (work task, async to readiness)
  → GET /gp/gpControl, single attempt, 2 s timeout
  → success: log model info, set slot model from JSON
  → failure: set slot model = HERO_LEGACY_RC

on_station_disassociated(mac)
  → clear wifi_ready, disarm timers
```

---

## Keepalive & WoL Watchdog

```
keepalive_timer fires every 3 s (per slot, armed after association)
  → rc_send_keepalive(ip)    — UDP unicast _GPHD_ to port 8484
  → check last_response_tick age
      age < 10 s    → nothing
      age >= 10 s   → arm wol_retry_timer

wol_retry_timer fires every 2 s
  → rc_send_wol(ip, mac)         — magic packet broadcast burst
  → rc_send_keepalive(ip)        — keepalive unicast
  → on next response (ACK or st), RX task refreshes last_response_tick;
    next keepalive_tick disarms wol_retry_timer
```

`last_response_tick` is written only by `rc_udp_rx_task` and read by the work task during keepalive tick. A 32-bit aligned `TickType_t` store is atomic on Xtensa LX7; no mutex is required.

---

## Source Files

| File | Responsibility |
|---|---|
| `include/gopro_wifi_rc.h` | Public API |
| `include/gopro_wifi_rc_spec.h` | Constants: ports, `_GPHD_` payload, opcode byte templates, response field offsets, timing, HTTP path strings (identify + datetime only) |
| `gopro_wifi_rc_internal.h` | Private types (`gopro_wifi_rc_ctx_t`, `rc_work_cmd_t`, `rc_shutter_cmd_t`), globals, function declarations |
| `driver.c` | Per-slot context table, work queue dispatch, global status-poll timer, vtable, public API |
| `connection.c` | Station lifecycle, **promote** (replaces probe), **HTTP identify**, keepalive tick, WoL retry, timer arm/disarm |
| `command.c` | Shutter task; `rc_http_get` (minimal — used only by identify + datetime); `rc_send_datetime` (HTTP); JSON substring extraction for identify |
| `status.c` | UDP status-poll handler; binary `st`-response parser |
| `udp.c` | Single bound socket on 8383; `rc_send_keepalive`, `rc_send_st`, `rc_send_sh`, `rc_send_wol`; RX dispatch (0x5F + `st` opcode) |
| `settings.c` | Placeholder — settings sub-commands not yet implemented |

`gopro_model_from_name()` lives in `gopro/gopro_model.c` (parent component), not here.

---

## Task Affinity

All three tasks pinned to **core 0** to share the WiFi/lwIP stack without cross-core cache invalidation.

| Task | Priority | Stack | Role |
|---|---|---|---|
| `rc_work_task` | 5 | 4 KB | Station lifecycle, identify, keepalive watchdog, status poll, datetime sync |
| `rc_shutter_task` | 7 | 4 KB | Shutter START/STOP — higher priority to minimise latency |
| `rc_udp_rx_task` | 4 | 2 KB | `recvfrom` on the shared 8383 socket; updates `last_response_tick`; dispatch by opcode |

---

## Known TODOs

| Location | Issue |
|---|---|
| `gopro_model.c` | `gopro_model_from_name()` lookup table needs Hero5 / Hero6 / Hero7 / Hero8 model_name → enum mappings. Pair-time identify logs the JSON `model_name` + `model_number` + `firmware_version` so unknown cameras can be added one at a time as hardware is observed. |
| `gopro_model_supports_http_datetime()` | Currently returns true only for HERO4_BLACK / HERO4_SILVER. Extend as Hero5+ STA-mode HTTP behaviour is verified. |
| `command.c` `rc_send_datetime` | Currently sends UTC; needs `can_manager_get_tz_offset()` applied before hex-byte encoding so the camera's clock reflects local time. |
| `command.c` `rc_send_datetime` | `CAMERA_MODEL_GOPRO_HERO_LEGACY_RC` has no working date/time path — Hero3-class doesn't run HTTP on its STA interface. Documented limitation. |
| Future opcodes | `YY` (clock/battery), `CM` (mode change), `PW` (power off) — documented in spec but not implemented. |

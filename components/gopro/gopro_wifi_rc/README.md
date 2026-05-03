# gopro_wifi_rc

Implements `camera_driver_t` for Hero 4 Black and Hero 4 Silver cameras using GoPro's older WiFi Remote emulation protocol. No BLE is involved — these cameras connect to the SoftAP automatically when they see the correct SSID (`HERO-RC-XXXXXX`) and MAC OUI (`d8:96:85`), both configured by `wifi_manager`.

Recording commands travel over plain HTTP/1.0 (port 80). Camera presence is maintained with a UDP keepalive/ACK exchange (ports 8484/8383). Cameras that have lost power are woken with Wake-on-LAN magic packets before the keepalive session starts.

---

## Responsibilities

- **Station lifecycle**: track Hero 4 cameras as they associate, receive a DHCP lease, and disconnect from the SoftAP.
- **Wake-on-LAN**: broadcast a 102-byte magic packet to the camera's MAC on first association if it already has a cached IP, retrying every 2 seconds until a UDP ACK is received.
- **UDP keepalive**: send `_GPHD_:0:0:2:0.000000\n` unicast to the camera on port 8484 every 3 seconds; receive ACK on port 8383 (first byte `0x5F`).
- **Keepalive watchdog**: if no ACK arrives for 10 seconds while the keepalive timer is running, re-arm the WoL retry timer.
- **HTTP probe**: after DHCP is assigned, verify the camera is responsive on port 80 before marking the slot WiFi-ready (up to 3 retries at 2-second intervals).
- **Shutter commands**: send `/gp/gpControl/command/shutter?p=1` and `?p=0` to all WiFi-ready slots (sequentially, high-priority shutter task).
- **Status polling**: poll `/gp/gpControl/status` every 5 seconds on all WiFi-ready slots; parse the `"8"` field from the `"status"` JSON object to update the recording state cache.
- **Datetime sync**: send the current UTC time to all WiFi-ready cameras via `/gp/gpControl/command/setup/date_time`. Internally gated on `can_manager_utc_is_session_synced()` — only fires after a live source (CAN GPS frame or web-UI manual set) has won this boot session. An NVS-restored UTC at boot leaves the camera's own clock untouched.

---

## Dependencies

```
REQUIRES: camera_manager, can_manager, wifi_manager, esp_timer, freertos, lwip, esp_wifi
```

**Precondition:** `camera_manager_init()` must be called before `gopro_wifi_rc_init()`.  
`gopro_wifi_rc_init()` must be called before `wifi_manager_set_callbacks()` and `wifi_manager_init()` — it sets up the queues and tasks that the station callbacks post into.

---

## Source Files

| File | Responsibility |
|------|---------------|
| `include/gopro_wifi_rc.h` | Public API |
| `include/gopro_wifi_rc_spec.h` | All raw constants: UDP ports, keepalive payload, HTTP paths, JSON field IDs, timing, task parameters |
| `gopro_wifi_rc_internal.h` | Private shared types (`gopro_wifi_rc_ctx_t`, `rc_work_cmd_t`, `rc_shutter_cmd_t`), globals, and internal function declarations |
| `driver.c` | Per-slot context table, work queue dispatch loop (`rc_work_task`), global status-poll timer, vtable, driver registration, public API |
| `connection.c` | Station lifecycle handlers, HTTP probe, keepalive tick (WoL watchdog), `esp_timer` arm/disarm helpers |
| `command.c` | `rc_http_get()` (raw BSD socket, HTTP/1.0), `rc_shutter_task()`, `rc_send_datetime()` |
| `status.c` | `rc_handle_status_poll_all()`, `parse_recording_status()` JSON field extractor |
| `udp.c` | Socket init, `rc_send_keepalive()`, `rc_send_wol()`, `rc_udp_rx_task()` |
| `settings.c` | NVS `last_ip` persistence (placeholder — not yet implemented) |

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
 * is internally a no-op until can_manager reports session-synced UTC. */
void gopro_wifi_rc_sync_time_all(void);
```

All station callbacks and `sync_time_all` post to `s_work_queue` and return immediately — safe to call from the WiFi event task.

---

## Transport Protocol

| Channel | Direction | Port | Protocol | Notes |
|---------|-----------|------|----------|-------|
| Keepalive TX | ESP32 → camera | 8484/UDP | UDP unicast | `_GPHD_:0:0:2:0.000000\n`, every 3 s |
| Keepalive ACK | camera → ESP32 | 8383/UDP | UDP | First byte `0x5F`; received by `rc_udp_rx_task` |
| Wake-on-LAN | ESP32 → broadcast | 9/UDP | UDP broadcast | 102-byte magic packet (6 × 0xFF + MAC × 16); burst of 5 |
| HTTP commands | ESP32 → camera | 80/TCP | HTTP/1.0 | Shutter, date/time — must be HTTP/1.0; Hero4 returns 500 on HTTP/1.1 |
| HTTP status | ESP32 → camera | 80/TCP | HTTP/1.0 | `/gp/gpControl/status` — response up to 4 KB |

---

## Connection Flow

```
on_station_associated(mac)
  known RC slot → post RC_CMD_STATION_ASSOCIATED
  unknown MAC   → silently ignored

RC_CMD_STATION_ASSOCIATED (work task)
  last_ip == 0  → wait for DHCP
  last_ip known → send WoL burst; arm keepalive timer; arm WoL retry timer

on_station_dhcp(mac, ip)
  known RC slot → store last_ip; post RC_CMD_PROBE

RC_CMD_PROBE (work task)
  → gopro_http_get("/gp/gpControl/status") × 3 attempts
  success       → set wifi_ready = true
                → camera_manager_on_camera_ready()
                → rc_send_datetime()
  all fail      → log error; slot stays not-ready

on_station_disassociated(mac)
  known RC slot → clear wifi_ready; disarm timers; clear last_keepalive_ack
```

---

## Keepalive & WoL Watchdog

```
keepalive_timer fires every 3 s (per slot, armed after association)
  → rc_send_keepalive(ip)    — UDP unicast to port 8484
  → check last_keepalive_ack age
      age < RC_KEEPALIVE_SILENCE_MS (10 s)  → nothing
      age >= 10 s                            → arm wol_retry_timer

wol_retry_timer fires every 2 s
  → rc_send_wol(ip, mac)   — magic packet broadcast burst
  → if ACK arrives on port 8383 → rc_udp_rx_task updates last_keepalive_ack
                                 → next keepalive_tick disarms wol_retry_timer
```

The `last_keepalive_ack` field is written only by `rc_udp_rx_task` and read by the work task during keepalive tick. A 32-bit aligned `TickType_t` store is atomic on Xtensa LX7; no mutex is required.

---

## Task Affinity

All three tasks are pinned to **core 0** to share the WiFi/lwIP stack without cross-core cache invalidation.

| Task | Priority | Stack | Role |
|------|----------|-------|------|
| `rc_work_task` | 5 | 4 KB | Station lifecycle, probe, keepalive watchdog, status poll, datetime sync |
| `rc_shutter_task` | 7 | 4 KB | Shutter START/STOP — higher priority to minimise latency |
| `rc_udp_rx_task` | 4 | 2 KB | `recvfrom` on port 8383; updates `last_keepalive_ack` |

---

## Known TODOs

| Location | Issue |
|----------|-------|
| `gopro_wifi_rc_spec.h` | Verify that JSON field `"30"` is the camera name on Hero 4; may need `/gp/gpControl/info` instead |
| `command.c` | `rc_send_datetime()` sends UTC only; no timezone offset applied from `can_manager_get_tz_offset()` |
| `gopro_wifi_rc.h` | `gopro_wifi_rc_add_camera()` defaults to `CAMERA_MODEL_GOPRO_HERO4_BLACK`; model picker in web UI is not yet implemented |
| `settings.c` | `last_ip` NVS persistence not yet implemented; cameras always start with `last_ip = 0` after reboot |

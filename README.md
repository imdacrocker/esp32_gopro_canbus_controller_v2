# ESP32 GoPro CAN-Bus Controller v2

An ESP32-S3 firmware that acts as a wireless RC controller for up to four GoPro cameras simultaneously. It presents itself as a GoPro WiFi Remote on both BLE and Wi-Fi, controls cameras over the Open GoPro API, and accepts control inputs from a CAN bus (e.g., a vehicle network or external button panel).

---

## Hardware

| Item | Detail |
|------|--------|
| SoC | ESP32-S3 |
| Flash | 8 MB |
| Partition table | Custom (`partitions.csv`) |
| CAN transceiver | TWAI-compatible (wired to ESP32-S3 TWAI peripheral) |

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                          app_main                               │
│  NVS init → netif init → event loop → components → AP ready    │
└────────┬──────────────────────────┬────────────────────────────┘
         │                          │
  ┌──────▼──────┐            ┌──────▼──────┐
  │  ble_core   │            │ wifi_manager│
  │  (core 1)   │            │  (core 0)   │
  │             │            │             │
  │ NimBLE host │            │ SoftAP      │
  │ scan/connect│            │ DHCP server │
  │ GATT writes │            │ station mgmt│
  └──────┬──────┘            └──────┬──────┘
         │                          │
  ┌──────▼──────┐     ┌─────────────┼─────────────┐
  │open_gopro_  │     │open_gopro_  │gopro_wifi_rc│
  │    ble      │     │    http     │  (Hero4)    │
  └──────┬──────┘     └──────┬──────┴──────┬──────┘
         │                   │             │
  ┌──────▼───────────────────▼─────────────▼──────┐
  │                  camera_manager                 │
  │      per-camera state, NVS slot records        │
  └──────────────────────┬─────────────────────────┘
                         │
                  ┌──────▼──────┐
                  │ can_manager │
                  │  TWAI node  │
                  │   RX task   │
                  └─────────────┘

┌──────────────────────────────────────────────────┐
│                  http_server                      │
│  esp_httpd · LittleFS /www · all /api/ handlers  │
│  (depends on all components above; nothing       │
│   depends on it — sits at the top of the graph)  │
└──────────────────────────────────────────────────┘
```

### Core Affinity

| Core | Tasks |
|------|-------|
| Core 0 | WiFi task, `esp_timer`, HTTP handlers |
| Core 1 | NimBLE host task, BT controller, `can_rx` task |

BLE and WiFi are pinned to opposite cores to minimize cache thrashing and radio coexistence latency.

---

## Components

| Component | Description |
|-----------|-------------|
| [`wifi_manager`](components/wifi_manager/README.md) | SoftAP with DHCP, MAC spoofing, station tracking |
| [`ble_core`](components/ble_core/README.md) | NimBLE scan/connect/encrypt/notify/bond management |
| [`camera_manager`](components/camera_manager) | Slot lifecycle, NVS records, driver vtable, mismatch correction |
| [`gopro/gopro_model.h`](components/gopro/gopro_model.h) | GoPro model capability helpers |
| [`open_gopro_ble`](components/gopro/open_gopro_ble/README.md) | Discovery, pairing, COHN provisioning, BLE keepalive |
| [`open_gopro_http`](components/gopro/open_gopro_http) | Open GoPro HTTPS/COHN driver (Hero 9+) |
| [`gopro_wifi_rc`](components/gopro/gopro_wifi_rc/README.md) | RC-emulation driver over WiFi (Hero 4) |
| [`can_manager`](components/can_manager/README.md) | TWAI node, 0x600/0x602 RX, 0x601 TX at 5 Hz, GPS UTC, timezone NVS |
| [`http_server`](components/http_server) | esp_httpd instance, LittleFS web UI, all `/api/` endpoint handlers |

---

## Boot Sequence

```c
app_main()
 1. nvs_flash_init()                    // Required for BLE bonding storage
 2. esp_netif_init()
 3. esp_event_loop_create_default()
 4. camera_manager_init()               // load NVS slot records
 5. open_gopro_http_init()              // register COHN HTTPS driver (Hero 9+)
 6. gopro_wifi_rc_init()                // register RC-emulation driver (Hero 4)
 7. open_gopro_ble_init()               // register BLE callbacks + purge bonds
 8. ble_core_init()                     // starts NimBLE host task; on_sync fires async
 9. can_manager_register_callbacks(...) // wire GPS UTC → sync_time_all callbacks
10. can_manager_init()                  // start TWAI node, RX task, TX timer, watchdog
11. wifi_manager_set_callbacks(...)     // wire station-associated/DHCP/disconnected CBs
12. wifi_manager_init()                 // Raise SoftAP (after all station CBs wired)
13. wifi_manager_wait_for_ap_ready()    // Block until beacon is on-air
14. http_server_init()                  // Mount LittleFS, start esp_httpd, register /api/ handlers
```

`ble_core_init()` and `wifi_manager_init()` overlap intentionally: NimBLE stack startup is asynchronous, so the AP can be raised while the BLE host is coming up. `http_server_init()` comes last because it depends on all other components being fully initialised.

---

## Getting Started

### Prerequisites

- [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
- Target: `esp32s3`

### Build

```bash
idf.py set-target esp32s3
idf.py build
```

If `sdkconfig` already exists from a previous build, delete it before rebuilding after any `sdkconfig.defaults` change:

```bash
rm sdkconfig && idf.py build
```

### Flash & Monitor

```bash
idf.py flash monitor
```

### Web UI

The web UI is served from the `storage` LittleFS partition (3 MB at offset `0x200000`). It is flashed separately from the firmware:

```bash
# Build the filesystem image from the www/ source files first, then:
parttool.py --port PORT write_partition --partition-name storage --input web_ui.bin
```

On first boot with a blank or corrupted storage partition, LittleFS formats automatically. The browser will show a placeholder page until the web UI image is flashed.

### Default Configuration

Project defaults live in [`sdkconfig.defaults`](sdkconfig.defaults). Key settings:

| Setting | Value | Reason |
|---------|-------|--------|
| `CONFIG_BT_NIMBLE_ENABLED` | `y` | NimBLE BLE host stack |
| `CONFIG_BT_NIMBLE_PINNED_TO_CORE` | `1` | BLE on core 1 |
| `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0` | `y` | WiFi on core 0 |
| `CONFIG_NIMBLE_MAX_CONNECTIONS` | `5` | 4 cameras + headroom |
| `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` | `517` | GoPro responses up to ~88 bytes |
| `CONFIG_BT_NIMBLE_NVS_PERSIST` | `y` | Bond storage survives reboot |
| `CONFIG_LWIP_DHCPS_MAX_STATION_NUM` | `8` | DHCP pool headroom |
| `CONFIG_LWIP_MAX_SOCKETS` | `25` | 16 httpd + 3 internal + 2 UDP (RC) + 4 TLS (COHN) |

---

## Radio Coexistence

The firmware uses Espressif's software coexistence (`CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE`). Additional measures applied at runtime:

- WiFi AP fixed to **channel 11** (2462 MHz) — clear of BLE advertising channels 37 (2402 MHz), 38 (2426 MHz), and 39 (2480 MHz).
- WiFi bandwidth forced to **HT20** (20 MHz) to avoid the upper half of a 40 MHz channel overlapping BLE.
- WiFi power save disabled (`WIFI_PS_NONE`) to keep the radio active during BLE scan windows.

---

## Network Configuration

The SoftAP uses a fixed address scheme so HTTP connections to cameras are predictable without DNS:

| Item | Value |
|------|-------|
| AP IP | `10.71.79.1` |
| Subnet | `10.71.79.0/24` |
| DHCP pool | `10.71.79.2` – `10.71.79.50` |
| SSID | `HERO-RC-XXXXXX` (last 3 MAC bytes) |
| Auth | Open (no password) |
| MAC OUI | `d8:96:85` (GoPro WiFi Remote OUI) |

The OUI spoof causes GoPro cameras to treat this device as a known remote type during pairing.

---

## HTTP API

The web UI is served from `http://10.71.79.1/`. All API endpoints return `application/json`.

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/logging-state` | RaceCapture logging state (`logging` / `not_logging` / `unknown`) |
| GET | `/api/utc` | GPS UTC timestamp with timezone offset applied |
| GET | `/api/auto-control` | Whether cameras follow CAN logging state automatically |
| POST | `/api/auto-control` | Set auto-control on/off |
| GET | `/api/paired-cameras` | All configured camera slots with connection and recording status |
| POST | `/api/shutter` | Start or stop recording (all cameras or a specific slot) |
| POST | `/api/remove-camera` | Remove a paired camera (compacts slot array) |
| POST | `/api/reorder-cameras` | Reorder camera slots (cameras must be disconnected first) |
| GET | `/api/cameras` | BLE scan results (GoPro cameras advertising `0xFEA6`) |
| POST | `/api/scan` | Start BLE discovery scan |
| POST | `/api/scan-cancel` | Cancel BLE discovery scan |
| POST | `/api/pair` | Initiate BLE pairing with a discovered camera |
| GET | `/api/rc/discovered` | SoftAP stations not yet registered as RC-emulation cameras |
| POST | `/api/rc/add` | Register a SoftAP station as an RC-emulation (Hero 4) camera |
| GET | `/api/settings/timezone` | Current UTC offset in whole hours |
| POST | `/api/settings/timezone` | Set UTC offset (`−12` to `+14`) |
| POST | `/api/settings/datetime` | Set system time manually (only when GPS not yet acquired) |
| POST | `/api/reboot` | Restart the ESP32 |
| POST | `/api/factory-reset` | Erase NVS and restart |

Full request/response contracts are in [`web_ui_spec.md`](web_ui_spec.md).

---

## License

See [LICENSE](LICENSE).

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
  ┌──────▼──────┐            ┌──────▼──────┐
  │open_gopro_  │            │open_gopro_  │
  │ ble (TODO)  │            │http (TODO)  │
  └──────┬──────┘            └──────┬──────┘
         │                          │
  ┌──────▼──────────────────────────▼──────┐
  │            camera_manager               │
  │   per-camera state, NVS slot records   │
  └──────────────────────┬─────────────────┘
                         │
                  ┌──────▼──────┐
                  │ can_manager │
                  │   (TODO)    │
                  │ TWAI RX task│
                  └─────────────┘
```

### Core Affinity

| Core | Tasks |
|------|-------|
| Core 0 | WiFi task, `esp_timer`, HTTP handlers |
| Core 1 | NimBLE host task, BT controller |

BLE and WiFi are pinned to opposite cores to minimize cache thrashing and radio coexistence latency.

---

## Components

| Component | Status | Description |
|-----------|--------|-------------|
| [`wifi_manager`](components/wifi_manager/README.md) | Done | SoftAP with DHCP, MAC spoofing, station tracking |
| [`ble_core`](components/ble_core/README.md) | Done | NimBLE scan/connect/encrypt/notify/bond management |
| [`camera_manager`](components/camera_manager) | Done | Slot lifecycle, NVS records, driver vtable, mismatch correction |
| [`gopro/gopro_model.h`](components/gopro/gopro_model.h) | Done | GoPro model capability helpers |
| [`open_gopro_ble`](components/gopro/open_gopro_ble/README.md) | Done | Discovery, pairing, COHN provisioning, BLE keepalive |
| `open_gopro_http` | TODO | Open GoPro HTTPS/COHN driver |
| `gopro_wifi_rc` | TODO | RC-emulation driver over WiFi |
| `can_manager` | TODO | TWAI driver and CAN message RX task |

---

## Boot Sequence

```c
app_main()
 1. nvs_flash_init()                    // Required for BLE bonding storage
 2. esp_netif_init()
 3. esp_event_loop_create_default()
 4. camera_manager_init()               // TODO: load NVS slot records
 5. open_gopro_http_init()              // TODO: register COHN HTTPS driver
 6. gopro_wifi_rc_init()                // TODO: register RC-emulation driver
 7. open_gopro_ble_init()               // register BLE callbacks + purge bonds
 8. ble_core_init()                     // Starts NimBLE host task; on_sync fires async
 9. can_manager_init()                  // TODO: start TWAI driver and RX task
10. wifi_manager_init()                 // Raise SoftAP (after all station CBs wired)
11. wifi_manager_wait_for_ap_ready()    // Block until beacon is on-air
```

`ble_core_init()` and `wifi_manager_init()` overlap intentionally: NimBLE stack startup is asynchronous, so the AP can be raised while the BLE host is coming up.

---

## Getting Started

### Prerequisites

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
- Target: `esp32s3`

### Build

```bash
idf.py set-target esp32s3
idf.py build
```

### Flash & Monitor

```bash
idf.py flash monitor
```

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

## License

See [LICENSE](LICENSE).

# ESP32 GoPro Controller — V2 Full Redesign

**Status:** Implemented
**Date:** 2026-04-26

---

## 1. Goals

- **Manufacturer-agnostic camera manager.** The core manager layer knows only about slots, model IDs, connection states, and a driver vtable. No GoPro-specific concepts leak into it.
- Replace BLE as the primary control transport with COHN (Camera on Home Network) over HTTPS, enabling concurrent command dispatch to all cameras.
- Use BLE solely for initial provisioning (wake camera, enable WiFi, obtain COHN credentials) and as a live fallback to re-provision WiFi if a camera drops off the network.
- Unify COHN cameras and RC-emulation cameras (Hero4, etc.) into a single slot structure with no per-type divergence in the manager layer. All behavioral branching gated on `camera_model_t` via named capability helpers.
- Persist COHN credentials and last-known IP in NVS so cameras are immediately usable on every subsequent boot without re-pairing.
- Clean component separation: WiFi plumbing, HTTP/web UI, GoPro BLE provisioning, GoPro HTTP control, and GoPro RC-emulation are independent components with explicit dependencies.
- Web UI served from a LittleFS data partition with pre-compressed assets, independently flashable from firmware.

---

## 2. Component Structure

```
components/
  camera_manager/         Manufacturer-agnostic: slots, NVS generic record,
                          coarse status enums, driver vtable dispatch
  can_manager/            Unchanged
  ble_core/               Generic BLE: scan, connect, bond, event callbacks
  wifi_manager/           SoftAP init, WiFi/IP event handling, DHCP station table
  http_server/            HTTP server, all /api/ handlers, serves web UI from LittleFS
  gopro/
    gopro_model.h         GoPro-specific camera_model_t values + capability helpers
    open_gopro_ble/       COHN provisioning via BLE; owns cam_N/gopro_cohn NVS key
    open_gopro_http/      COHN HTTPS driver; implements camera_driver_t vtable
    gopro_wifi_rc/        WiFi Remote emulation + UDP driver; implements camera_driver_t
```

### Dependency direction

```
http_server  →  camera_manager, ble_core, wifi_manager, gopro/*
gopro/*      →  camera_manager, ble_core, wifi_manager
camera_manager → ble_core (handle types only)
wifi_manager →  (no camera dependencies)
ble_core     →  (no camera dependencies)
can_manager  →  camera_manager
```

`wifi_manager` and `ble_core` are pure infrastructure — they know nothing about cameras or GoPro. `http_server` sits at the top of the dependency graph and is the only component that reaches across all others to service API requests.

### Responsibilities

**`camera_manager`** — Slot lifecycle, generic NVS record (`cam_N/camera`), tick timer, recording dispatch, state-change callbacks. Knows `camera_model_t` values but calls no GoPro capability helpers directly.

**`ble_core`** — Generic BLE scanning, connecting, bonding. Fires callbacks for connect/disconnect/data events. No GoPro awareness.

**`wifi_manager`** — SoftAP initialisation, WiFi and IP event handling, DHCP station table. Fires callbacks for station associate/connect/disconnect. No camera awareness.

**`http_server`** — HTTP server startup, all `/api/` endpoint handlers, serves `index.html` / `app.js.gz` / `style.css.gz` from LittleFS. Depends on all other components but is not depended upon by any.

**`open_gopro_ble`** — GoPro-specific BLE provisioning: set datetime, create COHN certificate, retrieve credentials. Stores `cohn_user` and `cohn_pass` in `cam_N/gopro_cohn` (its own NVS key, not camera_manager's record). Holds the BLE connection open after provisioning as a WiFi fallback.

**`open_gopro_http`** — Implements `camera_driver_t` for COHN cameras. HTTPS unicast to camera IP with Basic Auth. No TLS certificate verification (SoftAP is a private closed network; Basic Auth credentials provide sufficient protection).

**`gopro_wifi_rc`** — Implements `camera_driver_t` for RC-emulation cameras. WiFi Remote AP emulation. Keepalive sent over UDP; shutter and status commands sent over HTTP (exact parameters TBD during implementation).

---

## 3. sdkconfig.defaults

The values below should be committed in `sdkconfig.defaults` so every developer's build is consistent. Anything not listed here uses the ESP-IDF default for the target chip. The full file is reproduced at the end of this section as a single copy-pasteable block.

### 3.1 Target and partition

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="8MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
```

The custom partition table reserves a 3 MB `storage` partition for the LittleFS web UI assets (§19.1).

### 3.2 BLE / NimBLE

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y

# Pin NimBLE host and BT controller to core 1 (§4.1)
CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y
CONFIG_BT_CTRL_PINNED_TO_CORE_1=y

# Connection capacity: 4 cameras + headroom
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=5

# Bonding storage in NVS
CONFIG_BT_NIMBLE_NVS_PERSIST=y

# ATT MTU — request the maximum so GoPro long responses
# (GetHardwareInfoRsp ~88 bytes) do not fragment
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517
```

### 3.3 WiFi

```
# Pin WiFi task to core 0, opposite NimBLE (§4.1)
CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y

# DHCP server
CONFIG_LWIP_DHCPS=y
CONFIG_LWIP_DHCPS_MAX_STATION_NUM=8

```

### 3.4 HTTP / HTTPS

```
# Web UI server
# HTTPD_TASK_STACK_SIZE and HTTPD_MAX_OPEN_SOCKETS removed in IDF v6 — set via httpd_config_t in http_server_init()
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024

# HTTPS client (open_gopro_http)
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_MBEDTLS_TLS_CLIENT=y
CONFIG_MBEDTLS_HAVE_TIME=y
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
```

### 3.5 esp_timer

```
# Pin esp_timer task to core 0 alongside WiFi (§4.1)
CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0=y
CONFIG_ESP_TIMER_TASK_STACK_SIZE=4096
```

### 3.6 Logging

```
CONFIG_LOG_DEFAULT_LEVEL_INFO=y       # production-quiet at boot
CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y    # ceiling for runtime override (§22.6)
CONFIG_LOG_TIMESTAMP_SOURCE_RTOS=y
CONFIG_LOG_COLORS=y                   # ANSI colors over USB serial
```

### 3.7 Coexistence

```
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
```

Enabled by default when both BLE and WiFi are compiled in. Documented here so it does not get accidentally disabled.

### 3.8 Verification at boot

A short utility in `app_main()` (or the first component init) should log the actual core affinity and channel choice on every boot, so a misconfigured sdkconfig is visible immediately rather than as mysterious latency:

```c
ESP_LOGI(TAG, "boot: NimBLE core=%d, WiFi core=%d, channel=%d",
         CONFIG_BT_NIMBLE_PINNED_TO_CORE,
         CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0 ? 0 : 1,
         AP_CHANNEL);
```

`CONFIG_ESP_WIFI_TASK_CORE_ID` is a Kconfig `choice` name and does not generate a C integer macro. Use the boolean option directly. `CONFIG_BT_NIMBLE_PINNED_TO_CORE` is an explicit `int` config and resolves to 0 or 1 directly. `AP_CHANNEL` is `#define`d in `main.c` (temporarily) and will move to `wifi_manager.h`.

If those numbers ever surprise you on a new build, the `sdkconfig` overrode the defaults — investigate before chasing radio symptoms.

---

## 4. BLE / WiFi Coexistence and Core Pinning

The ESP32-S3 has two cores and a single 2.4 GHz radio shared between BLE and WiFi via TDMA arbitration in the BT controller. We are using NimBLE. The principle: pin WiFi to the **opposite** core from NimBLE so the two stacks do not contend for CPU cycles. They will still contend for the radio via TDMA, but that contention is unavoidable and is managed by the BT controller.

### 4.1 Task pinning — pin WiFi opposite NimBLE

NimBLE is pinned to core 1 in `sdkconfig` (§3.2). Everything else follows from that:

| Task | Configuration | Core | Rationale |
|---|---|---|---|
| NimBLE host | `CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y` | 1 | The reference point — all other pinning is relative to this |
| BT controller | `CONFIG_BT_CTRL_PINNED_TO_CORE_1=y` | 1 | Same core as NimBLE host; the controller posts events to it |
| WiFi task | `CONFIG_ESP_WIFI_TASK_CORE_ID_0=y` | 0 | Opposite NimBLE — no CPU contention with BLE |
| `esp_timer` task | `CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0=y` | 0 | Most timers fire WiFi/HTTP work; keep them adjacent |
| TWAI ISR | (default) | 0 | CAN ISR is light; default is fine |
| `can_manager` RX task | `xTaskCreatePinnedToCore` core 1 | 1 | Pure CPU parser; pin away from WiFi (sits next to BLE — CAN load is light) |
| `gopro_wifi_rc` work + shutter tasks | core 0 | 0 | Talk to UDP + HTTP via WiFi; keep them adjacent |
| `open_gopro_http` work task | core 0 | 0 | HTTPS via WiFi |
| `open_gopro_http` shutter one-shots | core 0 | 0 | Same reason |

Net effect: BLE work runs on core 1, WiFi work runs on core 0. They contend for the radio at the controller layer but not for CPU.

If at some point we move NimBLE to core 0, every WiFi-related row above flips to core 1. The pinning rule is "WiFi opposite NimBLE", not literally "WiFi on core 0".

### 4.2 SoftAP channel choice

BLE advertising channels are 37 (2402 MHz), 38 (2426 MHz), and 39 (2480 MHz). The SoftAP channel should overlap none of them.

| Channel | Center freq | Overlap with BLE adv |
|---|---|---|
| 1 | 2412 MHz | Adv 37 (2402) — close |
| 6 | 2437 MHz | Adv 38 (2426) — close |
| 11 | 2462 MHz | None — clear of all three primary advs |

**Use channel 11.** This is the §11.2 default. Channel 11 also avoids the worst of BT classic SCO collisions if a phone is ever paired to the controller (unlikely, but cheap insurance).

### 4.3 Concurrency hot-paths

The COHN provisioning sequence is the obvious risk window: heavy BLE GATT activity while the camera is joining the SoftAP. Symptoms of coexistence pressure look like BLE notification timeouts mid-provisioning, or DHCP requests that never receive a response. If observed in testing:

1. Verify task pinning is actually applied (`uxTaskGetSystemState()` reports the core for each task).
2. Check `CONFIG_BT_CTRL_COEX_PHY_CODED_TX_RX_TIME_LIMIT` and related coex tuning — but don't reach for these speculatively.
3. Consider serialising provisioning: only one COHN-pending camera is allowed to be joining the SoftAP at a time. Implementable as a queue in `open_gopro_ble`.

The radio is shared; 100% reliability under simultaneous BLE+WiFi heavy traffic is not achievable. Design for graceful degradation — every state machine that depends on BLE or WiFi should tolerate single-message drops and recover on the next event.

---

## 5. Model Identification

Camera model is the single source of truth for all behavioral branching. `camera_model_t` is defined in `camera_manager` and is manufacturer-agnostic in the sense that it holds values from all manufacturers. GoPro-specific capability helpers live in `gopro/gopro_model.h` and are only called by GoPro components — never by `camera_manager` directly.

### 5.1 `camera_model_t`

```c
typedef enum {
    CAMERA_MODEL_UNKNOWN           = 0,   /* Unset — invalid for NVS storage            */

    /* GoPro RC-emulation (IDs are project-assigned guesses — no official published spec.
       Selected manually by the user at pairing time; cannot be read from the camera.)    */
    CAMERA_MODEL_GOPRO_HERO4_BLACK  = 40,
    CAMERA_MODEL_GOPRO_HERO4_SILVER = 41,

    /* GoPro COHN (IDs verified against OpenGoPro GetHardwareInfo responses)             */
    CAMERA_MODEL_GOPRO_HERO9_BLACK  = 55,
    CAMERA_MODEL_GOPRO_HERO10_BLACK = 57,
    CAMERA_MODEL_GOPRO_HERO11_BLACK = 58,
    CAMERA_MODEL_GOPRO_HERO11_MINI  = 60,
    CAMERA_MODEL_GOPRO_HERO12_BLACK = 62,
    CAMERA_MODEL_GOPRO_MAX2         = 64,
    CAMERA_MODEL_GOPRO_HERO13_BLACK = 65,
    CAMERA_MODEL_GOPRO_LIT_HERO     = 70,

    /* Future manufacturer blocks: 1000+                                                */
} camera_model_t;
```

GoPro RC-emulation IDs are project-assigned and may need updating if official IDs are ever published. GoPro COHN IDs are verified against real `GetHardwareInfoRsp` payloads. Future manufacturers use the 1000+ block to avoid collisions.

### 5.2 GoPro capability helpers (`gopro/gopro_model.h`)

These helpers live in the GoPro component tree and are called only by GoPro components. `camera_manager` never calls them.

Each helper enumerates every known model explicitly rather than using numeric ranges. This is intentional: a new model ID must be consciously added to each applicable helper — there is no silent auto-inclusion based on a range. Adding a new manufacturer never requires touching these helpers, because 1000+ values will never match any GoPro check.

```c
/** True if the model is any known GoPro camera (RC-emulation or COHN).                 */
static inline bool gopro_model_is_gopro(camera_model_t model) {
    return gopro_model_uses_rc_emulation(model) || gopro_model_uses_cohn(model);
}

/** Camera connects by emulating a GoPro WiFi Remote AP.                                 */
static inline bool gopro_model_uses_rc_emulation(camera_model_t model) {
    return model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER;
}

/** Camera is controlled via COHN HTTPS after joining the SoftAP.                       */
static inline bool gopro_model_uses_cohn(camera_model_t model) {
    return model == CAMERA_MODEL_GOPRO_HERO9_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO10_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_MINI
        || model == CAMERA_MODEL_GOPRO_HERO12_BLACK
        || model == CAMERA_MODEL_GOPRO_MAX2
        || model == CAMERA_MODEL_GOPRO_HERO13_BLACK
        || model == CAMERA_MODEL_GOPRO_LIT_HERO;
}

/** Keepalive must be sent over UDP. Other commands (shutter, status) use HTTP.          */
static inline bool gopro_model_uses_udp_keepalive(camera_model_t model) {
    return gopro_model_uses_rc_emulation(model);
}

/** Model cannot be read from the camera — must be selected by the user at pairing.     */
static inline bool gopro_model_requires_manual_id(camera_model_t model) {
    return gopro_model_uses_rc_emulation(model);
}
```

`gopro_model_requires_manual_id` and `gopro_model_uses_rc_emulation` share the same implementation today but are intentionally separate — the questions they answer may diverge as more models are added.

Additional helpers will be added as behavioral differences are confirmed during testing.

---

## 6. NVS Layout

Each camera occupies namespace `cam_N` (N = slot index). Multiple keys coexist in the same namespace — camera_manager owns one, manufacturer components own their own.

### 6.1 Generic record — `cam_N/camera` (owned by `camera_manager`)

```c
typedef struct {
    uint8_t       version;       /* Schema version — currently 1                        */
    char          name[32];      /* Human-readable camera name                          */
    camera_model_t model;        /* Must not be CAMERA_MODEL_UNKNOWN at save time       */
    uint8_t       mac[6];        /* BLE addr for COHN cameras; WiFi MAC for RC-emulation*/
    uint32_t      last_ip;       /* Last DHCP-assigned IP; updated on each connection   */
    bool          is_configured;
} camera_nv_record_t;
```

**Schema versioning policy:** A blob whose `version` does not match the current schema is discarded; the slot is left unconfigured and re-pairing is required. Firmware upgrades do not automatically invalidate NVS — only a version bump does.

**Save-time validation:** `camera_manager_save_slot()` returns `ESP_ERR_INVALID_ARG` and logs an error if `model == CAMERA_MODEL_UNKNOWN`.

### 6.2 GoPro COHN credentials — `cam_N/gopro_cohn` (owned by `open_gopro_ble`)

```c
typedef struct {
    char cohn_user[32];   /* COHN Basic Auth username                                   */
    char cohn_pass[64];   /* COHN Basic Auth password                                   */
} gopro_cohn_nv_record_t;
```

Written once during initial BLE provisioning. `open_gopro_ble` reads and writes this key independently using the slot index as a namespace coordinate. `camera_manager` never touches it.

RC-emulation cameras have no entry under this key.

---

## 7. In-RAM Structure

### 7.1 `camera_slot_t`

```c
typedef struct {
    /* --- Persisted fields (mirrored from cam_N/camera on load) --- */
    char           name[32];
    camera_model_t model;
    uint8_t        mac[6];
    uint32_t       last_ip;      /* Updated in NVS each time DHCP assigns a new IP      */
    bool           is_configured;

    /* --- BLE state (coarse — detail lives in open_gopro_ble) --- */
    cam_ble_status_t ble_status;
    uint16_t         ble_handle; /* BLE_HS_CONN_HANDLE_NONE when not connected          */

    /* --- WiFi / network state --- */
    wifi_cam_status_t wifi_status;
    uint32_t          ip_addr;   /* Current DHCP IP; may differ from last_ip            */

    /* --- Control --- */
    desired_recording_t    desired_recording;
    const camera_driver_t *driver;
    void                  *driver_ctx;
} camera_slot_t;
```

### 7.2 `cam_ble_status_t`

Camera manager sees only coarse BLE states. Detailed GoPro-specific states (bonded, subscribed, provisioning) are internal to `open_gopro_ble` and mapped to this enum when reported upward.

```c
typedef enum {
    CAM_BLE_NONE = 0,       /* RC-emulation camera, or COHN camera not yet contacted   */
    CAM_BLE_CONNECTING,     /* Any in-progress BLE work: scan, connect, bond, provision */
    CAM_BLE_CONNECTED,      /* L2 up, manufacturer-specific setup in progress           */
    CAM_BLE_READY,          /* Setup complete; held open as WiFi re-provision fallback  */
} cam_ble_status_t;
```

### 7.3 `wifi_cam_status_t`

```c
typedef enum {
    WIFI_CAM_NONE = 0,      /* Not on network                                          */
    WIFI_CAM_ASSOCIATING,   /* L2 association in progress                              */
    WIFI_CAM_ASSOCIATED,    /* L2 up, waiting for DHCP lease                          */
    WIFI_CAM_CONNECTED,     /* IP assigned by ESP32 DHCP server                       */
    WIFI_CAM_PROBING,       /* Sending test HTTPS/UDP command to verify responsiveness */
    WIFI_CAM_READY,         /* Camera confirmed ready for recording commands           */
} wifi_cam_status_t;
```

**Operational readiness:** `wifi_status == WIFI_CAM_READY` is the sole gate for `start_recording()` / `stop_recording()`. Replaces the old `camera_ready` bool and `wifi_connected` bool.

### 7.4 `camera_recording_status_t`

Reported by every driver via `get_recording_status()`. Three states only — there is intentionally no `STARTING` or `STOPPING` state. The "command in flight" case is handled by the mismatch-correction grace period (§13.4); the type system does not need to reflect it.

```c
typedef enum {
    CAMERA_RECORDING_UNKNOWN = 0,   /* Driver has not yet observed the camera, or the
                                       last status request failed. Mismatch correction
                                       is suppressed in this state.                      */
    CAMERA_RECORDING_IDLE,          /* Camera is connected and not recording.            */
    CAMERA_RECORDING_ACTIVE,        /* Camera is actively recording.                     */
} camera_recording_status_t;
```

`UNKNOWN` is the initial value and is also the value returned after a status poll error or transport failure. The mismatch-correction loop (§13.4) treats `UNKNOWN` as a no-op — it never issues a corrective command against an unknown status, because doing so would risk re-issuing the same command repeatedly during a transient failure.

### 7.5 `desired_recording_t`

Per-slot recording intent. Stored in `camera_slot_t` and consulted by the mismatch-correction loop.

```c
typedef enum {
    DESIRED_RECORDING_UNKNOWN = 0,  /* No intent established yet. Set at boot and held
                                       until the first CAN logging frame arrives (when
                                       auto-control is enabled) or the UI explicitly sets
                                       a state (when auto-control is disabled).
                                       Mismatch correction is suppressed in this state — 
                                       the system does not issue start or stop commands
                                       until it knows what the operator intends.          */
    DESIRED_RECORDING_STOP    = 1,  /* Intent: camera should not be recording.           */
    DESIRED_RECORDING_START   = 2,  /* Intent: camera should be recording.               */
} desired_recording_t;
```

`DESIRED_RECORDING_UNKNOWN` is distinct from `DESIRED_RECORDING_STOP`. A `STOP` intent actively commands cameras to stop recording. `UNKNOWN` means "we have not yet heard from the CAN bus or the UI — take no action." This prevents the controller from issuing stop commands to cameras that were already recording when the ESP32 rebooted, before the first CAN frame re-establishes intent.

---

## 8. Driver Vtable

`camera_driver_t` signature is unchanged. Driver selection at slot load time uses `camera_model_t` directly:

```c
camera_driver_type_t drv_type = gopro_model_uses_rc_emulation(rec.model)
    ? CAMERA_DRIVER_GOPRO_RC_UDP
    : CAMERA_DRIVER_GOPRO_COHN_HTTPS;
```

| Driver | Component | Transport |
|---|---|---|
| `CAMERA_DRIVER_GOPRO_COHN_HTTPS` | `open_gopro_http` | `esp_http_client` HTTPS + Basic Auth |
| `CAMERA_DRIVER_GOPRO_RC` | `gopro_wifi_rc` | UDP (keepalive) + HTTP (shutter, status) |

`camera_manager_register_new()` accepts a `camera_model_t` parameter. For COHN cameras this comes from `GetHardwareInfo` over BLE. For RC-emulation cameras it comes from the user's selection in the web UI pairing flow.

```c
struct camera_driver {
    esp_err_t (*start_recording)(void *ctx);
    esp_err_t (*stop_recording)(void *ctx);
    camera_recording_status_t (*get_recording_status)(void *ctx); /* returns last known cached value */
    void (*teardown)(void *ctx);                                  /* nullable — see §20.5 */
    void (*update_slot_index)(void *ctx, int new_slot);           /* nullable — update cached slot */
};
```

`get_recording_status()` is a non-blocking cache read, safe to call from any context. How and when a driver refreshes that cache is an internal implementation detail — `camera_manager` does not need to know or care.

`teardown()` is called by `camera_manager_remove_slot()` and gives the driver a chance to clean up its own resources (close persistent HTTPS connections, erase manufacturer-specific NVS keys, free memory). It may be `NULL` for drivers that have nothing to clean up beyond the slot record.

---

## 9. Public Info Struct

`camera_slot_info_t` is returned by `camera_manager_get_slot_info()` to the web UI. The CAN manager reads camera state via `camera_manager_get_slot_can_state()` (see §14.2) and never touches this struct.

```c
typedef struct {
    /* Identity */
    int            index;
    char           name[32];
    camera_model_t model;
    uint8_t        mac[6];
    bool           is_configured;

    /* Granular connection state — for web UI */
    cam_ble_status_t  ble_status;
    wifi_cam_status_t wifi_status;

    /* Operational state */
    bool                  is_recording;
    desired_recording_t   desired_recording;
    uint32_t              ip_addr;          /* 0 when not connected */

    /* Future additions: battery_pct, storage_remaining_mb, temperature, etc. */
} camera_slot_info_t;
```

`is_recording` is derived from `wifi_status == WIFI_CAM_READY && driver->get_recording_status() == CAMERA_RECORDING_ACTIVE`. `desired_recording` is exposed so the web UI can show whether the operator intends the camera to be recording, which may differ from its actual state during a transition.

---

## 10. Network Topology

The ESP32 runs a SoftAP that serves as the home network for all cameras:

- **COHN cameras** are provisioned via BLE to connect to the SoftAP SSID. Once connected, HTTPS is used for all recording commands.
- **RC-emulation cameras** (Hero4 Black, Hero4 Silver) treat the same SoftAP as a GoPro WiFi Remote AP and connect automatically when they see the correct SSID. Recording commands are sent via UDP.

Because the ESP32 is the DHCP server, camera IP addresses are known the moment a DHCP lease fires — no BLE query is needed to discover the current IP on reconnection.

### Connection Workflow — COHN Cameras

```
BLE scan → connect → bond → subscribe to notifications
  → open_gopro_ble: set datetime, create cert, retrieve cohn_user + cohn_pass
  → save credentials to cam_N/gopro_cohn (open_gopro_ble's NVS key)
  → camera joins SoftAP → DHCP event fires → ip_addr known
  → update last_ip in cam_N/camera
  → open_gopro_http: send HTTPS probe → WIFI_CAM_READY
  → BLE remains connected as fallback
```

On every subsequent boot (credentials already in NVS):

```
BLE connect (fallback ready) → camera joins SoftAP → DHCP event fires
  → ip_addr known → update last_ip in NVS
  → open_gopro_http: send HTTPS probe → WIFI_CAM_READY
```

### Connection Workflow — RC-Emulation Cameras

```
Camera associates with SoftAP (recognises WiFi Remote SSID)
  → DHCP event fires → ip_addr known → update last_ip in NVS
  → gopro_wifi_rc: UDP/HTTP probe confirms camera responds
  → WIFI_CAM_READY
```

On reconnect, `last_ip` from NVS is used as the initial target since RC-emulation cameras may not request a new DHCP lease. RC-emulation cameras cannot report their own model — `gopro_wifi_rc_add_camera()` defaults to `CAMERA_MODEL_GOPRO_HERO4_BLACK`; no model picker is present in the web UI. `camera_manager_save_slot()` returns `ESP_ERR_INVALID_ARG` if `model == CAMERA_MODEL_UNKNOWN`.

### Command Transport Summary

RC-emulation and COHN cameras use mixed transports depending on command type.

| Command | COHN cameras | RC-emulation cameras |
|---|---|---|
| Keepalive | HTTPS `GET /gopro/camera/keepalive` | UDP `_GPHD_:0:0:2:0.000000\n` → port 8484 |
| Shutter start/stop | HTTPS `GET /gopro/camera/shutter/{start\|stop}` | HTTP `GET /gp/gpControl/command/shutter?p=1` / `?p=0` |
| Status request | HTTPS `GET /gopro/camera/state` | HTTP `GET /gp/gpControl/status` |
| Date/time | BLE `SetDateTime` (§16) then HTTPS once COHN ready | HTTP `GET /gp/gpControl/command/setup/date_time?p=...` (raw HTTP/1.0) |

### Recording Command Dispatch

The camera manager fires `start_recording()` / `stop_recording()` on all ready slots in a tight loop via the driver vtable — effectively concurrent. Each driver handles its own transport internally.

---

## 11. WiFi Manager

### 11.1 Responsibilities

`wifi_manager` is a pure infrastructure component. Its sole responsibilities are:

- Bring up the ESP32 SoftAP with the correct SSID, IP, and radio settings.
- Maintain a station table (MAC → IP mapping) for all currently-associated SoftAP clients.
- Forward WIFI and IP events to registered callbacks so higher-level components can react without knowing anything about the underlying Wi-Fi stack.

`wifi_manager` has **no camera awareness** and **no HTTP server**. It does not import `camera_manager`, `open_gopro_ble`, `gopro_wifi_rc`, `ble_core`, or `can_manager`.

### 11.2 SoftAP Configuration

| Parameter | Value | Notes |
|---|---|---|
| SSID | `HERO-RC-XXXXXX` | Last 3 bytes of chip MAC, e.g. `HERO-RC-A1B2C3` |
| IP address | `10.71.79.1` | Fixed; set before `esp_wifi_start()` |
| Gateway | `10.71.79.1` | Same as IP |
| Subnet mask | `255.255.255.0` | `/24` |
| DHCP pool | `10.71.79.2 – 10.71.79.50` | 49 addresses; cameras use option-50 preferred-IP on reconnect |
| Channel | 11 | 2462 MHz; clear of all three primary BLE advertising channels (37/38/39 = 2402/2426/2480 MHz). See §4 for coexistence rationale. |
| Auth mode | Open (`WIFI_AUTH_OPEN`) | No password; RC-emulation cameras require open AP |
| Max connections | 6 | 4 camera slots + 1 setup device + 1 spare |
| Power save | `WIFI_PS_NONE` | Disabled at init and re-applied on `WIFI_EVENT_AP_STOP` restart |
| Inactive time | 60 s | Default is 300 s; shortened so a silently-dropped camera is evicted and deauth'd cleanly in 15 s |

**Inactive time note:** The 60-second value was chosen because keepalive packets are sent every 2–3 seconds. Any camera that is actually alive will elicit a response that resets the inactivity timer. A camera that has gone silent is cleanly evicted in ≤60 s, triggering a fresh DHCP handshake on reconnect rather than getting stuck in a stale association.

**HT20 bandwidth + PMF disabled:** Bandwidth is forced to HT20 (`esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20)`) on `WIFI_EVENT_AP_START`. PMF (Protected Management Frames) is explicitly disabled because some iOS builds attempt an OWE handshake on certain IDF builds with WPA3 compiled in, which prevents association.

### 11.3 MAC Spoofing

The AP MAC OUI is overridden to `d8:96:85` so the ESP32 presents as a GoPro WiFi Remote device. Hero4 (and other RC-emulation) cameras auto-connect to an AP whose SSID is `HERO-RC-XXXXXX` **and** whose source MAC matches this OUI. The last three bytes are preserved from the factory MAC to keep the address unique per device.

```c
uint8_t gopro_mac[6] = { 0xd8, 0x96, 0x85, mac[3], mac[4], mac[5] };
esp_wifi_set_mac(WIFI_IF_AP, gopro_mac);
```

**Known ESP-IDF v5.x issue:** `0xD8` (binary `1101 1000`) does not have bit 1 set (the "locally administered" bit). Newer IDF builds validate this bit and return `ESP_ERR_WIFI_MAC`. If the override is rejected, the AP starts with the factory OUI and Hero4 RC-emulation cameras will not auto-connect. The warning is logged and `gopro_wifi_rc` must detect this case at startup. Workaround options: patch IDF validation, use a different MAC byte that satisfies the locally-administered check, or apply MAC spoofing via a different mechanism.

### 11.4 Station Table

```c
typedef struct {
    bool     active;
    uint8_t  mac[6];
    uint32_t ip_addr;   /* 0 until DHCP lease fires */
} sta_entry_t;

static sta_entry_t s_stations[AP_MAX_CONN];
```

The table is written from WiFi/IP event callbacks and read from HTTP handlers and camera components via the public API. Access is lock-free — minor races are acceptable because the table is only used for display and device discovery, not for safety-critical operations.

**Hero4 IP behaviour:** Hero4 cameras perform a standard DHCP request on their very first connection to a new SoftAP. The DHCP server assigns an IP from the pool and fires `IP_EVENT_ASSIGNED_IP_TO_CLIENT`; `gopro_wifi_rc` records this as `last_ip` and persists it via `camera_manager_save_slot()`. On subsequent reconnections, the Hero4 re-associates without requesting a new DHCP lease — it reuses the IP it learned on the first connection. Because no DHCP event fires on reconnect, `gopro_wifi_rc` relies on `last_ip` from NVS to address the camera. This is why `last_ip` is persisted on every confirmed DHCP assignment.

### 11.5 Event Handling

**`WIFI_EVENT_AP_START`**
- Apply HT20 bandwidth (`esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20)`)
- Set `AP_STARTED_BIT` in the event group to unblock `wifi_manager_wait_for_ap_ready()`

**`WIFI_EVENT_AP_STOP`**
- Log warning ("AP stopped unexpectedly")
- Clear `AP_STARTED_BIT`
- Call `esp_wifi_start()` to restart; re-apply `WIFI_PS_NONE`

**`WIFI_EVENT_AP_STACONNECTED`**
- Add station to table (`ip_addr` starts at 0)
- Fire `on_station_associated(mac)` callback

**`WIFI_EVENT_AP_STADISCONNECTED`**
- Remove station from table
- Fire `on_station_disconnected(mac)` callback

**`IP_EVENT_ASSIGNED_IP_TO_CLIENT`**
- Update station table entry with assigned IP
- Fire `on_station_ip_assigned(mac, ip)` callback

### 11.6 Callbacks

`wifi_manager` exposes station events via registered callbacks. It does not call into camera components directly.

```c
typedef void (*wifi_mgr_station_associated_cb_t)(const uint8_t mac[6]);
typedef void (*wifi_mgr_station_disconnected_cb_t)(const uint8_t mac[6]);
typedef void (*wifi_mgr_station_ip_assigned_cb_t)(const uint8_t mac[6], uint32_t ip);

void wifi_manager_set_callbacks(
    wifi_mgr_station_associated_cb_t  on_associated,
    wifi_mgr_station_disconnected_cb_t on_disconnected,
    wifi_mgr_station_ip_assigned_cb_t  on_ip_assigned
);
```

`gopro_wifi_rc` registers these callbacks at init. `http_server` does not need them (it queries the station table directly when serving `GET /api/rc/discovered`).

### 11.7 Public API

```c
/** Bring up the SoftAP, configure DHCP, apply radio settings, start the AP. */
void wifi_manager_init(void);

/** Block until WIFI_EVENT_AP_START fires (or AP_READY_TIMEOUT_MS = 5000 ms elapses).
 *  Called by the startup sequence before BLE init to ensure the beacon is on air
 *  before starting radio-intensive BLE work. */
void wifi_manager_wait_for_ap_ready(void);

/** Return the current IP of a station by MAC, or 0 if not found / no DHCP yet. */
uint32_t wifi_manager_get_station_ip(const uint8_t mac[6]);

/** Copy up to max_count currently-associated station entries into out[].
 *  Returns the number of entries written. */
int wifi_manager_get_connected_stations(wifi_mgr_sta_info_t *out, int max_count);

/** Register station event callbacks (called once at system init). */
void wifi_manager_set_callbacks(
    wifi_mgr_station_associated_cb_t,
    wifi_mgr_station_disconnected_cb_t,
    wifi_mgr_station_ip_assigned_cb_t
);
```

### 11.8 What `wifi_manager` Does NOT Own

| Item | Owned by |
|---|---|
| HTTP server startup and all `/api/` handlers | `http_server` |
| `index.html` / web assets | `web_ui/` (project root) |
| `GET /api/rc/discovered` handler | `http_server` (reads station table via `wifi_manager_get_connected_stations()`) |
| RC-emulation camera event routing | `gopro_wifi_rc` (via registered callbacks) |
| COHN camera event routing | `open_gopro_http` / `open_gopro_ble` (via registered callbacks) |

---

## 12. BLE Core

### 12.1 Responsibilities

`ble_core` owns the NimBLE host task, the GAP scan/connect lifecycle, and bond management. It is fully camera-agnostic — it knows nothing about GoPro, COHN, or slot numbers. Higher layers register a callback struct and are notified of every relevant event.

`ble_core` does not import `camera_manager`, `open_gopro_ble`, `wifi_manager`, or any camera driver. The only direction of coupling is upward: callers register callbacks, and `ble_core` invokes them.

### 12.2 Threading Model

A single FreeRTOS task (`ble_host_task`) runs `nimble_port_run()` and processes the NimBLE event queue. All NimBLE API calls (`ble_gap_connect`, `ble_gap_disc`, `ble_store_*`, etc.) **must** run on this task. Calls originating from other tasks (HTTP handlers, camera_manager timers) are safe because `ble_core` routes them through the NimBLE event queue via `ble_npl_eventq_put()` before executing. All callbacks fire on the NimBLE host task — implementations must not block indefinitely.

### 12.3 Initialisation

```
1. ble_core_register_callbacks()   ← must be called first; callbacks copied by value
2. ble_core_init()
     - nimble_port_init()
     - ble_hs_cfg: sync_cb = on_sync, reset_cb = on_reset
     - Security manager: sm_io_cap = NO_IO, sm_bonding = 1
     - Key distribution: ENC + ID (both directions)
     - ble_svc_gap_init(), device name = "ESP32 Controller"
     - nimble_port_freertos_init(ble_host_task)
3. Stack fires on_sync when ready → boot reconnect chain starts automatically
```

**Pairing mode:** Just Works (no I/O capability). No PIN or confirmation is required. Bonding is enabled so long-term keys (LTK) are stored in NimBLE's NVS peer-security store and reused on every subsequent connection.

### 12.4 Scan Modes

Two scan modes are used, both passive (no scan requests sent to peers):

**Background scan** — runs indefinitely until all known cameras are connected.
- Parameters: `BLE_HS_FOREVER`, `BLE_HCI_SCAN_FILT_USE_WL` (hardware whitelist filters non-target advertisements at the controller)
- `filter_duplicates` disabled — a camera that comes back online after a gap must be detected even though its address was seen before the scan started
- `on_disc` callback is **not** invoked; advertisements only reach the scan event handler for reconnect processing
- Suppressed entirely via `start_scan_if_needed()` when `has_disconnected_cameras()` returns false

**Discovery scan** — user-initiated, runs for 120 seconds.
- Parameters: 120 000 ms timeout, `filter_policy = 0` (no whitelist — must see all cameras including unpaired ones), `filter_duplicates = 0`
- Every advertisement packet that parses successfully is forwarded to the `on_disc` callback; `open_gopro_ble` applies its own GoPro UUID filter
- Cancelled immediately by `ble_core_stop_discovery()`, which then restarts the background scan if needed
- `BLE_GAP_EVENT_DISC_COMPLETE` fires at end of 120 s and transitions back to background scan automatically

**Scan mutual exclusion:** Only one scan or connect can be active at a time. `ble_core_connect_by_addr()` cancels any running scan before calling `ble_gap_connect()`. `ble_core_start_discovery()` cancels the background scan before starting the discovery scan.

### 12.5 Boot Reconnect

On `on_sync`, `ble_core` calls `start_scan_if_needed()`. That is the entire boot reconnect sequence.

The passive background scan with `is_known_addr` already handles reconnection: when a known camera advertises, the scan handler sees it, calls `ble_gap_connect()`, and the normal connection event flow proceeds. No explicit iteration of bonded peers, no sequential chain, no `BLE_HS_FOREVER` stall.

### 12.6 Connection Events

All GAP events are handled in `connection_event_cb` (defined in `ble_connect.c`, shared across the module via `ble_core_internal.h`).

**`BLE_GAP_EVENT_CONNECT` (success)**
- Fires `on_connected(conn_handle, addr)` — higher layer stores handle, sets `CAM_BLE_CONNECTED`
- Immediately calls `ble_gap_security_initiate()` to resume encryption with stored LTK (or begin first-time pairing)
- Clears `s_connecting` flag

**`BLE_GAP_EVENT_CONNECT` (failure)**
- Clears `s_connecting` flag
- `start_scan_if_needed()` resumes the background scan

**`BLE_GAP_EVENT_ENC_CHANGE` (success)**
- Fires `on_encrypted(conn_handle, addr)` — this is the correct point for `open_gopro_ble` to begin GATT service discovery and subscribe to notifications. GATT commands sent before encryption is established will be rejected by the GoPro.

**`BLE_GAP_EVENT_ENC_CHANGE` (failure)**
- Distinguishes transient failures from genuine key mismatches:

| Error codes | Classification | Action |
|---|---|---|
| `BLE_HS_ETIMEOUT`, `BLE_HS_ETIMEOUT_HCI`, `BLE_HS_ENOTCONN`, `BLE_HS_ECONTROLLER` | Transient | Bond preserved; scan resumes via disconnect handler |
| All other status codes | Key mismatch | Bond deleted from NVS; next connection will perform fresh SMP pairing |

Deleting a bond on a transient failure would cause desynchronised bond state: the camera still has a bond but the ESP32 does not. The camera would then reject the fresh pairing request and the two sides could diverge permanently until the user re-pairs manually. The distinction above prevents this.

**`BLE_GAP_EVENT_REPEAT_PAIRING`**
- Symmetric case: the camera has a bond but the ESP32 does not (e.g. after an NVS erase)
- Deletes the camera's stale bond entry and returns `BLE_GAP_REPEAT_PAIRING_RETRY` so NimBLE retries fresh pairing on the same connection

**`BLE_GAP_EVENT_DISCONNECT`**
- Fires `on_disconnected(conn_handle, addr, reason)` before clearing state — higher layer must look up the slot by `conn_handle` while it is still valid
- Clears `s_connecting` flag
- Calls `start_scan_if_needed()` to resume the background scan

**`BLE_GAP_EVENT_NOTIFY_RX`**
- Copies notification payload into a 512-byte stack buffer (matches maximum negotiated ATT MTU; previous 64-byte limit silently dropped long responses such as `GetHardwareInfoRsp` at ~88 bytes)
- Fires `on_notify_rx(conn_handle, attr_handle, data, len)` — `open_gopro_ble` dispatches by `attr_handle` to the appropriate response handler

### 12.7 Bond Management

**`ble_core_purge_unknown_bonds(keep[], keep_count)`**
- Called **once at startup** from `open_gopro_ble_init()`, before `ble_core_init()` starts the NimBLE host task. Because no cameras can connect before the host task is running, the purge executes without any connection race. The background scan (`start_scan_if_needed()`) does not start until `on_sync` fires — which is after init returns — so the purge is guaranteed complete before the first advertisement is processed.
- Walks the peer-security store, collects addresses not in `keep[]`, deletes them.
- Must not be called after `ble_core_init()` starts the NimBLE host task; it is not posted to the event queue and is not safe from other contexts.

**`ble_core_remove_bond(addr)`**
- Terminates the active connection to `addr` (if any) via `BLE_ERR_REM_USER_CONN_TERM`
- Deletes `addr`'s entry from the peer-security store
- Posts to the NimBLE event queue — safe to call from HTTP handler tasks
- Caller must remove the camera from `camera_manager` *before* calling this, so that `is_known_addr` returns false and the disconnect handler does not trigger an automatic reconnect

### 12.8 GATT Write

```c
esp_err_t ble_core_gatt_write(uint16_t conn_handle, uint16_t attr_handle,
                               const uint8_t *data, uint16_t len);
```

Write Without Response (ATT write command). Non-blocking on success. Used by `open_gopro_ble` to send OpenGoPro TLV commands to the camera.

### 12.9 Callback Table

Registered once via `ble_core_register_callbacks()` before `ble_core_init()`. Copied by value — the caller does not need to keep the struct alive.

| Field | Type | Implementor | Purpose |
|---|---|---|---|
| `on_disc` | `ble_core_on_disc_cb_t` | `open_gopro_ble` | Advertisement seen during discovery scan |
| `on_connected` | `ble_core_on_connected_cb_t` | `open_gopro_ble` / `camera_manager` | L2 link up, before encryption |
| `on_encrypted` | `ble_core_on_encrypted_cb_t` | `open_gopro_ble` | Encryption established — begin GATT discovery |
| `on_disconnected` | `ble_core_on_disconnected_cb_t` | `open_gopro_ble` / `camera_manager` | Link dropped |
| `on_notify_rx` | `ble_core_on_notify_rx_cb_t` | `open_gopro_ble` | ATT notification/indication received |
| `is_known_addr` | `ble_core_is_known_addr_cb_t` | `camera_manager` | Returns true if addr is a paired camera; gates auto-reconnect in scan handler |
| `has_disconnected_cameras` | `ble_core_has_disconnected_cameras_cb_t` | `camera_manager` | Returns true if any paired camera is not connected; gates background scan start |

All callbacks are optional (NULL-safe). All fire on the NimBLE host task.

### 12.10 Source File Layout

| File | Contents |
|---|---|
| `ble_init.c` | `ble_core_init()`, `on_sync` boot reconnect, `ble_core_register_callbacks()` |
| `ble_scan.c` | Background and discovery scan management, `ble_core_connect_by_addr()`, `scan_event_cb` |
| `ble_connect.c` | `connection_event_cb`, bond purge/remove, boot reconnect chain helpers |
| `ble_gatt_write.c` | `ble_core_gatt_write()` |
| `ble_core_internal.h` | Shared state and internal function declarations across the four source files |

---

## 13. Recording State Management

### 13.1 Per-slot desired state

Each camera slot holds a `desired_recording` value of type `desired_recording_t` (§7.5). This is the authoritative intent for that camera — independent of what the camera is actually doing at any given moment.

`desired_recording` is never persisted to NVS. It always resets to `DESIRED_RECORDING_UNKNOWN` on boot. The mismatch-correction loop takes no action while the value is `UNKNOWN` — no start or stop commands are issued until the CAN bus or the web UI establishes an explicit intent. See §13.2 for how intent is established after boot.

### 13.2 Automatic control (CAN-driven)

When automatic control is enabled (`camera_manager_get_auto_control() == true`), **every** received `0x600` frame calls:

```c
camera_manager_set_desired_recording_all(desired_recording_t intent);
/* intent is DESIRED_RECORDING_START or DESIRED_RECORDING_STOP based on the frame payload */
```

This is intentionally idempotent — set on every frame, not only on transitions. A re-stated `DESIRED_RECORDING_START` while every camera is already `CAMERA_RECORDING_ACTIVE` produces no output (the mismatch loop in §13.4 has nothing to correct). The reason for setting on every frame instead of only on transitions is robustness against missed transitions:

- If the ESP32 reboots mid-race, the first `0x600` frame after boot re-establishes the correct intent without depending on transition detection.
- If a single `0x600` frame is dropped on the bus, the next frame reasserts the intent — no harm done.
- If a camera was offline when the transition occurred and connects later, its status poll will see the desired/actual divergence and issue the correct command on its own. The CAN side does not need to remember to re-send.

Default state at boot is `DESIRED_RECORDING_UNKNOWN` (§7.5 / §13.1). `camera_manager_set_desired_recording_all()` is **not called** while the CAN logging state is `LOGGING_STATE_UNKNOWN` (§14.2) — the `UNKNOWN` CAN state means the bus is silent or the RaceCapture has not yet sent a frame, and the system has no basis for issuing a command. The `desired_recording` for all slots stays at `UNKNOWN` until a real `0x600` frame arrives.

When automatic control is disabled, CAN frames do not modify `desired_recording`. The web UI controls each camera individually via:

```c
camera_manager_set_desired_recording_slot(int slot, desired_recording_t intent);
```

The UI must supply `DESIRED_RECORDING_START` or `DESIRED_RECORDING_STOP`; it may not set `UNKNOWN`. Once the UI sets an intent, the mismatch loop activates for that slot.

### 13.2.1 Reboot-mid-race recovery

The combination of "set on every CAN frame" and the per-camera status poll provides automatic recovery from an ESP32 reboot during an active recording session. After reboot:

1. `desired_recording` resets to `DESIRED_RECORDING_UNKNOWN` on every slot (§13.1). No shutter commands are sent until intent is established.
2. Cameras and CAN come back online over the next several seconds.
3. The first `0x600` frame received sets `desired_recording = DESIRED_RECORDING_START` on all slots if RaceCapture is still in the `LOGGING` state.
4. On the next status poll for each camera, the mismatch loop sees `DESIRED_RECORDING_START` against `CAMERA_RECORDING_IDLE` (cameras are not recording immediately after the ESP32 reboot — they had no command to start) and issues `start_recording()`.

If the RaceCapture logging state is `NOT_LOGGING` when the ESP32 reboots, intent is set to `DESIRED_RECORDING_STOP` and the mismatch loop does nothing further (cameras are already idle).

If no `0x600` frame arrives (CAN fault, RaceCapture powered off), `desired_recording` stays `UNKNOWN` indefinitely and no shutter commands are ever issued. The operator must either restore the CAN link or switch to manual control via the web UI.

Total recovery time is bounded by the CAN logging-state heartbeat (continuous at 1+ Hz from RaceCapture) plus one status poll cycle — well under 10 s.

### 13.3 Status polling

Each camera slot, once `WIFI_CAM_READY`, runs a periodic mismatch check driven by a per-slot `esp_timer` in `camera_manager`. The interval is per-model and defined in `gopro_model.h` (default 2 seconds; may be tuned as testing reveals differences).

On each timer fire, `camera_manager` calls `driver->get_recording_status(ctx)`, which returns the driver's **last known cached value** immediately — no network request is made. A few seconds of lag between the camera's actual state and the cached value is acceptable in this application.

Each driver is responsible for keeping its own cache fresh through whatever internal mechanism suits its transport:

| Camera category | Cache update mechanism |
|---|---|
| COHN | Driver's internal task periodically calls `GET https://{ip}/gopro/camera/state` — recording field TBD (varies by model) |
| RC-emulation | Driver's internal task periodically calls HTTP GET to camera status endpoint — parameters TBD during implementation |

The per-slot timer is stopped when the slot leaves `WIFI_CAM_READY` and restarted when it re-enters it.

### 13.4 Mismatch correction

Immediately after each status update, `camera_manager` compares the driver's cached recording status against `desired_recording`:

```
if desired_recording == UNKNOWN                               → no-op (no intent established)
if status == UNKNOWN                                          → no-op (camera state unknown)
if grace_period_active                                        → no-op (command in flight)
if desired_recording == START  && status == IDLE              → call driver->start_recording()
                                                                 set grace_period_active = true
if desired_recording == STOP   && status == ACTIVE            → call driver->stop_recording()
                                                                 set grace_period_active = true
```

**Grace period.** After issuing `start_recording()` or `stop_recording()`, `camera_manager` sets a per-slot `grace_period_active` flag. The flag is cleared on the next poll after it was set, regardless of whether the cache reflects the new state yet. This gives the camera one full poll cycle (≥ status poll interval) for the command to take effect and the cache to update before the mismatch loop fires another correction. Without this grace period a slow camera could receive several duplicate `start_recording` commands while the first is still being processed.

The grace period applies only to the next mismatch decision after a correction; it does not block subsequent corrections beyond that. If the second poll still shows mismatch, a fresh correction is sent. This is the self-healing safety net for the "missed shutter" case.

**UNKNOWN suppresses correction (both sides).** If `desired_recording == DESIRED_RECORDING_UNKNOWN`, the system has no established intent and takes no action — this is the boot state before the first CAN frame or UI command. If `status == CAMERA_RECORDING_UNKNOWN`, the driver has not yet observed the camera and correction is suppressed to avoid re-issuing commands repeatedly during a transient transport failure when the camera might already be in the desired state.

**Note:** if a camera cannot record for hardware reasons (no SD card, storage full, overheating), the mismatch will persist indefinitely and a correction command will be sent on every poll cycle. This is acceptable for the current use case — these are operator errors in a race context. A retry counter or error-state detection can be added later if log spam becomes a problem.

### 13.5 Vtable

```c
struct camera_driver {
    esp_err_t (*start_recording)(void *ctx);
    esp_err_t (*stop_recording)(void *ctx);
    camera_recording_status_t (*get_recording_status)(void *ctx); /* returns last known cached value */
    void (*teardown)(void *ctx);                                  /* nullable — see §20.5 */
};
```

`get_recording_status()` is a non-blocking cache read, safe to call from any context. How and when a driver refreshes that cache is an internal implementation detail — `camera_manager` does not need to know or care.

`teardown()` is called by `camera_manager_remove_slot()` and gives the driver a chance to clean up its own resources (close persistent HTTPS connections, erase manufacturer-specific NVS keys, free memory). It may be `NULL` for drivers that have nothing to clean up beyond the slot record.

---

## 14. CAN Manager

### 14.1 Hardware

| Parameter | Value |
|---|---|
| Baud rate | 1 Mbps |
| TX GPIO | 7 |
| RX GPIO | 6 |
| RX queue depth | 32 frames |
| TX queue depth | 8 frames |
| Termination | Hardware (120 Ω solder jumpers on board — enabled by default for end-node use) |

### 14.2 CAN Protocol

All frames use standard 11-bit IDs.

**`0x600` — RaceCapture → ESP32: logging command**

Payload byte 0 is the `isLogging` flag. The manager fires the `can_logging_state_cb_t` callback on **every received frame**, passing `LOGGING_STATE_LOGGING` or `LOGGING_STATE_NOT_LOGGING` based on the byte. The callback consumer (`camera_manager`) is responsible for idempotent handling — see §13.2.

State defaults to `LOGGING_STATE_UNKNOWN` on boot. The first received `0x600` frame transitions out of `UNKNOWN`. If no `0x600` frame is received within 5 seconds at any point during operation, the state reverts to `LOGGING_STATE_UNKNOWN` and the callback is **not** fired with `UNKNOWN` — `UNKNOWN` is only used internally and reflected in `can_manager_get_logging_state()` for the web UI. The 5 s timeout covers RaceCapture power-off, cable fault, and bus-off conditions.

**`0x601` — ESP32 → RaceCapture: camera status broadcast**

Transmitted at 5 Hz (every 200 ms) by a periodic `esp_timer` callback regardless of bus activity. Bytes 0–3 carry `CAMERA_STATE_*` values for camera slots 0–3:

```
CAMERA_STATE_UNDEFINED    = 0   Slot not configured / no information yet
CAMERA_STATE_DISCONNECTED = 1   Camera not found or connection lost
CAMERA_STATE_IDLE         = 2   Connected, not recording
CAMERA_STATE_RECORDING    = 3   Connected and actively recording
```

Values are unsigned integers mapped directly to RaceCapture direct-CAN channels. The mapping must not be reordered without updating the RaceCapture configuration.

`can_manager` reads the current state for each slot using:

```c
camera_can_state_t camera_manager_get_slot_can_state(int slot);
```

This function is defined in `camera_manager` and translates the slot's `wifi_status` and `recording_status` into the appropriate `CAMERA_STATE_*` value, keeping the translation logic co-located with the data it reads. `can_manager` calls it directly when building the `0x601` payload — it does not go through `camera_slot_info_t`.

**`0x602` — RaceCapture → ESP32: GPS UTC timestamp**

A 64-bit little-endian Unix epoch in milliseconds, broadcast by a RaceCapture Lua script at 25 Hz once GPS lock is acquired. The first valid frame (year > 2020) fires the `can_utc_acquired_cb_t` callback exactly once — used to set the date and time on all currently-connected cameras. Subsequent frames update the stored epoch and the monotonic clock reference used for extrapolation.

`can_manager_get_utc_ms()` uses the last received timestamp plus elapsed `esp_timer_get_time()` to return the current estimated UTC without waiting for the next CAN frame. Returns `false` until the first valid frame arrives.

### 14.3 Timezone Offset

`can_manager_set_tz_offset(int8_t hours)` stores a UTC offset (clamped to the IANA valid range of −12 to +14 hours) in NVS. This offset is applied when setting camera date/time so recorded clips have correct local timestamps. The value is loaded from NVS on `can_manager_init()` before any camera clock operations.

### 14.4 Threading Model

A single FreeRTOS task (`s_rx_task`, priority 5) dequeues frames from `s_rx_queue` and processes them. The TWAI ISR enqueues raw frames. Camera state bytes are `volatile uint8_t` — single-byte writes are atomic on Xtensa LX7 without a mutex. UTC state is protected by a mutex since it is a multi-field structure read from multiple tasks.

### 14.5 Callbacks

| Callback | Fires when |
|---|---|
| `can_rx_frame_cb_t` | Every received frame — for development and bus sniffing |
| `can_logging_state_cb_t` | Every received `0x600` frame (consumer is responsible for idempotency — see §13.2) |
| `can_utc_acquired_cb_t` | Exactly once, on the first valid `0x602` frame |

All callbacks are invoked from the RX task context. Implementations must not block indefinitely.

---

## 15. open_gopro_ble

### 15.1 Responsibilities

`open_gopro_ble` handles everything GoPro-specific that requires BLE — and only that. Recording start/stop, recording status polling, video preset loading, and `SetCameraControlStatus` all live in `open_gopro_http` and travel over HTTPS. BLE is used exclusively for provisioning and fallback re-provisioning.

- **Discovery**: scan for cameras advertising the GoPro service UUID (`0xFEA6`); populate the discovered list for the web UI.
- **Provisioning**: after GATT setup, extract the camera's model ID (`GetHardwareInfo`), set the clock (`SetDateTime`), and — once valid UTC is available — provision COHN (configure the camera to connect to the SoftAP and retrieve its Basic Auth credentials). `SetCameraControlStatus` is sent by `open_gopro_http` via HTTPS after COHN is established.
- **Fallback**: hold the BLE connection open after provisioning completes so the camera can be re-provisioned over BLE if its COHN credentials become stale or its SoftAP entry is lost.
- **BLE keepalive**: send the OpenGoPro keep-alive command (0x5B) every 3 seconds, independently of the HTTPS keepalive, to prevent the camera auto-sleeping and to maintain the BLE link supervision timer. The BLE keepalive is sent to the `settings_write` characteristic (GP-0074) with a single payload byte `0x42`; the response arrives on `settings_resp_notify` (GP-0075) and is acknowledged but not acted upon. This keepalive is necessary even when COHN is active — the BLE link supervision timeout is independent of WiFi/HTTP activity, and the BLE connection must stay alive for re-provisioning fallback.

### 15.2 Discovery

The `on_disc` callback (registered with `ble_core`) filters advertisements by the GoPro BLE service UUID `0xFEA6`. Devices that match are added to a 10-entry discovery list (`gopro_device_t[]`) with name, address, and RSSI. The list is cleared when a new scan starts.

The `http_server` component reads this list via `open_gopro_ble_get_discovered()` when serving `GET /api/cameras`.

### 15.3 GATT Characteristics

All handles are 128-bit GoPro UUIDs of the form `b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b`:

| Handle name | UUID suffix | Direction | Purpose |
|---|---|---|---|
| `cmd_write` | `0072` | Write | Command write |
| `cmd_resp_notify` | `0073` | Notify | Command responses |
| `settings_write` | `0074` | Write | Settings write |
| `settings_resp_notify` | `0075` | Notify | Settings responses |
| `query_write` | `0076` | Write | Query write |
| `query_resp_notify` | `0077` | Notify | Query responses |
| `net_mgmt_cmd_write` | `0091` | Write | Network management commands (COHN provisioning) |
| `net_mgmt_resp_notify` | `0092` | Notify | Network management responses |
| `wifi_ssid_read` | `0002` | Read | Wi-Fi AP SSID (readable) |
| `wifi_pass_read` | `0003` | Read | Wi-Fi AP password (readable) |
| `wifi_power_write` | `0001` | Write | Wi-Fi AP power on/off |
| `wifi_state_indicate` | `0004` | Indicate | Wi-Fi AP state |

CCCD subscriptions are required on every connection — GoPro cameras do not persist CCCD state.

### 15.4 Per-Camera Driver Context (`gopro_ble_ctx_t`)

A heap-allocated context is created per slot by `open_gopro_ble_create_driver_ctx()` and stored in `camera_slot_t.driver_ctx`. It is passed to all vtable calls.

```c
typedef struct {
    uint16_t               conn_handle;
    gopro_gatt_handles_t   gatt;              /* zeroed on disconnect */

    /* Readiness poll state */
    bool                   readiness_polling;
    uint8_t                readiness_retry_count;
    esp_timer_handle_t     readiness_timer;   /* one-shot, 3s, retries GetHardwareInfo */

    /* COHN provisioning state */
    bool                   cohn_provisioning; /* true while COHN sequence is in progress */
    bool                   cohn_pending_utc;  /* COHN needed but UTC not yet available;
                                                 provisioning deferred until sync_time_all() fires */
} gopro_ble_ctx_t;
```

### 15.5 Connection State Machine

Every event on this path runs on the NimBLE host task.

```
on_connected(conn_handle, addr)
  -> camera_manager_find_by_addr(addr)
      known camera -> camera_manager_on_connected(slot, conn_handle)
                     store conn_handle in ctx
      unknown      -> ignored here; registered in on_encrypted below
  -> ble_gap_security_initiate() fires (in ble_core)

on_encrypted(conn_handle, addr)
  -> if unknown: camera_manager_register_new() with placeholder name
  -> start_gatt_discovery(conn_handle)
      1. Negotiate MTU — request the NimBLE-configured maximum (typically
         BLE_ATT_MTU_MAX). Read back the actual negotiated MTU from
         BLE_GAP_EVENT_MTU and store it in the slot context. All subsequent
         reads/writes must size against the negotiated MTU, never a hardcoded
         constant — the camera may negotiate down to 23 bytes and we must not
         silently truncate long responses (e.g. GetHardwareInfoRsp at ~88 bytes).
      2. ble_gattc_disc_all_svcs -> for each service:
         ble_gattc_disc_all_chrs -> record GP-00XX handles
      3. Subscribe to all notify/indicate CCCDs sequentially
      4. On all CCCDs done -> commit handles to gopro_ble_ctx_t
         -> gopro_start_readiness_poll(conn_handle)

gopro_start_readiness_poll
  -> send GetHardwareInfo (0x3C) to query_write (GP-0076)
  -> arm 3s one-shot readiness_timer
  -> on response (cmd_resp_notify, GP-0073):
      status 0x00 -> camera hardware ready -> gopro_on_camera_ready()
      non-zero    -> retry (up to 10 x 3s = ~30s) -> give up: ble_gap_terminate()
      timer fires -> same as non-zero

gopro_on_camera_ready()
  -> extract camera_model_t from GetHardwareInfo response
  -> camera_manager_set_model(slot, model)
  -> check cam_N/gopro_cohn in NVS:

      credentials present
        -> UTC available?
            yes -> SetDateTime (best-effort, no retry)
            no  -> skip (camera retains its own clock; sync deferred to sync_time_all())
        -> camera_manager_set_camera_ready(slot, true)
        -> BLE keepalive timer running (3 s periodic, GP-0074)
        -> recording commands dispatched via open_gopro_http (HTTPS)

      credentials absent OR re-provisioning requested
        -> UTC available?
            yes -> SetDateTime -> run COHN provisioning sequence (section 15.6)
            no  -> set ctx->cohn_pending_utc = true
                  BLE keepalive timer running (camera must not sleep while we wait)
                  camera sits in BLE-connected / not-ready state

open_gopro_ble_sync_time_all()   [called by CAN manager on GPS lock, or by web UI manual set]
  -> for every connected slot:
      send SetDateTime (best-effort)
      if ctx->cohn_pending_utc:
        clear cohn_pending_utc
        -> run COHN provisioning sequence (section 15.6)
```

### 15.6 COHN Provisioning Sequence

Run only when `cam_N/gopro_cohn` does not contain valid credentials, or when triggered by a re-provisioning event (repeated HTTPS 401 from `open_gopro_http`).

```
1. RequestCreateCOHNCert
     -> Protobuf command to net_mgmt_cmd_write (GP-0091)
     -> Response on net_mgmt_resp_notify (GP-0092)
     -> Camera generates a self-signed TLS cert
     -> We do NOT read the cert back (ssl.CERT_NONE on SoftAP private network)

2. RequestSetApEntries
     -> Provide our SoftAP SSID ("HERO-RC-XXXXXX") and empty password (open AP)
     -> Camera stores the AP credentials

3. RequestConnect (or camera auto-connects)
     -> Camera joins SoftAP
     -> wifi_manager fires on_station_ip_assigned callback
     -> gopro_wifi_rc / camera_manager updates last_ip for the slot

4. RequestGetCOHNStatus
     -> Poll until COHN state = CONNECTED (retry with backoff, ~10s total)
     -> Extract: username (string), password (string)

5. Save to NVS: cam_N/gopro_cohn { cohn_user, cohn_pass }
6. camera_manager sets wifi_status = WIFI_CAM_CONNECTED
   -> open_gopro_http probes the camera -> WIFI_CAM_READY
```

**Re-provisioning trigger**: `open_gopro_http` calls `open_gopro_ble_reprovision(slot)` after N consecutive HTTPS 401 responses (N TBD during implementation). This clears `cam_N/gopro_cohn` and runs the COHN sequence again on the existing BLE connection. If the BLE connection has been lost, `ble_core`'s background scan will reconnect it first.

### 15.7 Disconnect Cleanup

On `on_disconnected(conn_handle, addr, reason)` (fires before `camera_manager` clears the handle):

1. `gopro_readiness_cancel(conn_handle)` — stops and deletes `readiness_timer`
2. Clear `gopro_ble_ctx_t`: zero `gatt` handles, clear `recording_status`, `start_cmd_pending`, `cohn_provisioning`, `cohn_pending_utc`
3. `camera_manager_on_disconnected(conn_handle)` — clears `ble_handle`, sets `ble_status = CAM_BLE_NONE`
4. `free_gatt_disc_ctx(conn_handle)`, `gopro_query_free(conn_handle)`

Timer cleanup must happen before the driver context is touched to prevent stale timer callbacks.

### 15.8 NVS Ownership

`open_gopro_ble` owns the key `cam_N/gopro_cohn` exclusively:

```c
typedef struct {
    char cohn_user[32];
    char cohn_pass[64];
} gopro_cohn_nv_record_t;
```

`camera_manager` never reads or writes this key. `open_gopro_http` reads it (via a function exported from this component) to construct HTTPS Basic Auth headers.

### 15.9 Public API

```c
void open_gopro_ble_init(void);

/* Discovery */
void open_gopro_ble_start_discovery(void);
void open_gopro_ble_stop_discovery(void);
int  open_gopro_ble_get_discovered(gopro_device_t *out, int max_count);

/* Connection */
void open_gopro_ble_connect_by_addr(const ble_addr_t *addr);

/* COHN credentials — read by open_gopro_http */
bool open_gopro_ble_get_cohn_credentials(int slot,
                                          char *user_out, size_t user_len,
                                          char *pass_out, size_t pass_len);

/* Re-provisioning — called by open_gopro_http on repeated HTTPS 401 */
void open_gopro_ble_reprovision(int slot);

/* UTC sync — called from can_manager UTC-acquired callback */
void open_gopro_ble_sync_time_all(void);

/* Driver context lifecycle */
void *open_gopro_ble_create_driver_ctx(void);
const camera_driver_t *open_gopro_ble_get_driver(void);
```

### 15.10 Source File Layout

| File | Contents |
|---|---|
| `include/open_gopro_ble_spec.h` | Named constants, packet byte arrays, command IDs, response field offsets, and spec-URL comments for every BLE command and notification used by this component. This is the canonical reference layer — all `.c` files include it rather than embedding raw byte literals. |
| `driver.c` | `open_gopro_ble_init()`, vtable registration, `create_driver_ctx()` |
| `pairing.c` | `on_connected`, `on_encrypted`, `on_disconnected` callbacks |
| `gatt.c` | MTU negotiation, service/characteristic discovery, CCCD subscriptions |
| `readiness.c` | GetHardwareInfo poll; on success extracts `camera_model_t`, calls `gopro_on_camera_ready()` |
| `control.c` | SetDateTime, BLE keepalive timer |
| `cohn.c` *(new)* | COHN provisioning sequence, NVS read/write for `cam_N/gopro_cohn`, `reprovision()` |
| `query.c` | GPBS reassembly, command response dispatch by cmd_id |
| `notify.c` | ATT notification routing to query.c by characteristic handle |

The same pattern applies to `open_gopro_http`: an `include/open_gopro_http_spec.h` will serve as the canonical reference for all HTTPS endpoint paths, request/response field names, and spec-URL comments for that component. Documented when section 16 (open_gopro_http) is written.


---

## 16. open_gopro_http

HTTPS recording control driver for COHN-provisioned GoPro cameras. Implements the `camera_driver_t` vtable and owns all post-provisioning HTTP communication: connection sequence, shutter commands, status polling, keepalive, and re-provisioning triggers.

### 16.1 Responsibilities

| Responsibility | Notes |
|---|---|
| Implement `camera_driver_t` vtable | `start_recording`, `stop_recording`, `get_recording_status`, `teardown` |
| Connection sequence after COHN | Probe camera, send SetCameraControlStatus (EXTERNAL), load Video preset |
| Shutter commands | One-shot FreeRTOS task per camera per shutter event — true concurrency |
| Recording status poll | `GET /gopro/camera/state` every 5 s; result cached in `open_gopro_http_ctx_t` |
| Re-provision trigger | Calls `open_gopro_ble_reprovision(slot)` after `COHN_REPROVISION_THRESHOLD` consecutive 401 responses |

> **Keepalive split:** The BLE 0x5B keepalive (sent every 3 s by `open_gopro_ble` — see section 15.1) keeps both the camera awake and the BLE link supervision timer alive. `open_gopro_http` does **not** send a separate HTTP keepalive; the 5 s status poll (`GET /gopro/camera/state`) is sufficient to keep the HTTPS session warm and satisfies the GoPro's HTTP activity requirements. Two independent keepalive channels (BLE + HTTPS status poll) together ensure the camera neither sleeps nor drops the BLE fallback link.

### 16.2 HTTPS Transport and Trust Assumptions

Cameras are on the SoftAP at `10.71.79.x`. All requests use HTTPS with Basic Auth credentials from `cam_N/gopro_cohn` (read via `open_gopro_ble_get_cohn_credentials()`). TLS certificate verification is disabled (`skip_cert_common_name_check = true`, no CA cert).

**Trust model — explicit and deliberate:** the SoftAP is open authentication (no WPA2). Anyone within radio range of the controller can join. With TLS cert verification disabled, a malicious station on the same SoftAP could MITM a camera and read Basic Auth headers in cleartext. This is accepted because:

- RC-emulation cameras (Hero4) require an open-auth AP; we cannot use WPA2 without breaking them.
- The deployment context is a race vehicle. Physical proximity is required to be on the AP.
- Even if creds were stolen, the attacker can only start/stop recording — no destructive operation, no exfiltration.

**Do not assume the SoftAP is private.** Other components (especially `http_server`) should not store user secrets accessible over the SoftAP, and should not extend trust based on "the request came from a connected station."

One `esp_http_client` handle is maintained per camera slot. The handle is initialised during the connection sequence and reused across all requests on that slot to benefit from TLS session resumption — keeping the connection warm avoids paying the TLS handshake cost on every shutter.

All endpoint paths and field names are defined in `include/open_gopro_http_spec.h` with references to the OpenGoPro HTTP spec. No raw URL strings appear in `.c` files.

### 16.3 Per-Camera Driver Context (`open_gopro_http_ctx_t`)

```c
typedef struct {
    int                      slot;
    uint32_t                 last_ip;
    esp_http_client_handle_t client;
    SemaphoreHandle_t        client_mutex;

    camera_recording_status_t recording_status;

    bool                     http_ready;

    uint8_t                  consecutive_401s;
} open_gopro_http_ctx_t;
```

### 16.4 Threading Model

There is **one** persistent `esp_http_client` handle per slot, guarded by a per-slot mutex. Two task types contend for it:

**Work task** (`gopro_http_work`, priority 5, one task total):
- Single queue, depth 16
- Handles all non-shutter commands: probe, SetCameraControlStatus, load preset, status poll, IP update, disconnect cleanup
- Runs a periodic timer (5 s) that posts `CMD_POLL_ALL` to its own queue
- Acquires `ctx->client_mutex` for each HTTPS request, releases it after the response is consumed
- The status poll keeps the TLS session warm so shutter requests skip the handshake

**Shutter dispatch — one-shot FreeRTOS task per camera per shutter event** (priority 7):
- The vtable function `start_recording(ctx)` / `stop_recording(ctx)` does not block. It spawns a one-shot task pinned to the same core as the work task, with the action passed in via task parameter.
- The spawned task acquires `ctx->client_mutex`, fires the HTTPS request on the warm session, releases the mutex, then `vTaskDelete(NULL)`.
- N ready cameras: N tasks created in immediate succession — all running concurrently on FreeRTOS, each on its own stack. True parallelism, no serialisation.
- The vtable returns `ESP_OK` immediately after spawning. Failure to spawn is logged and returns `ESP_ERR_NO_MEM`; the shutter for that slot is dropped — the next mismatch poll will retry.

**Why mutex over two handles?** Each `esp_http_client` HTTPS handle holds a TLS session in heap (~30 KB). One handle per slot x 4 slots is already substantial. Two would double that. With one handle, the status poll keeps the connection warm for free. Mutex contention is bounded — FreeRTOS mutexes have priority inheritance so the high-priority shutter task boosts the work task to release quickly. Worst-case shutter latency from lock contention is approximately the status poll RTT, well inside race tolerance.

**Spawn cost:** `xTaskCreate` on ESP32 with a 4 KB stack runs in tens of microseconds. Spawning 4 tasks per shutter event (~once per race lap) is far below the cost of a single TLS handshake.

```
camera_manager / open_gopro_ble callbacks
  -> post CMD_CAMERA_READY(slot)        -> work queue
  -> post CMD_CAMERA_DISCONNECTED(slot) -> work queue
  -> post CMD_IP_UPDATED(slot, ip)      -> work queue

camera_driver_t vtable calls (from camera_manager)
  -> start_recording(ctx)       -> xTaskCreate(shutter_oneshot, ..., {ctx, START}, ...)
  -> stop_recording(ctx)        -> xTaskCreate(shutter_oneshot, ..., {ctx, STOP},  ...)
  -> get_recording_status(ctx)  -> read ctx->recording_status directly (no post, no lock)

esp_timer (5 s periodic) -> post CMD_POLL_ALL -> work queue
```

### 16.5 Connection Sequence

Posted as `CMD_CAMERA_READY(slot)` to the work queue when `camera_manager_set_camera_ready()` is called after COHN provisioning completes.

```
handle_camera_ready(slot)
  1. Fetch credentials: open_gopro_ble_get_cohn_credentials(slot, ...)
  2. Fetch IP: camera_manager_get_last_ip(slot)
  3. Initialise esp_http_client handle for slot (HTTPS, Basic Auth, skip cert check)
     Create ctx->client_mutex
     Store in ctx->client and ctx->last_ip
  4. Probe: GET /gopro/camera/state
       -> parse recording_status from response -> store in ctx->recording_status
       -> if probe fails: retry up to 3x with 2 s delay; give up -> log error, leave http_ready=false
  5. SetCameraControlStatus(EXTERNAL)
       -> non-200 -> log warning, proceed anyway
  6. Load Video preset
       -> GET /gopro/camera/presets/get -> find first Video group preset ID
       -> GET /gopro/camera/presets/set?id=N
       -> non-200 -> log warning, proceed anyway
  7. ctx->http_ready = true
  8. Log "slot N: HTTPS ready"
```

### 16.6 Shutter Command Flow

```
shutter_oneshot_task(params)
  ctx    = params->ctx
  action = params->action       /* SHUTTER_START or SHUTTER_STOP */

  1. if (!ctx->http_ready) -> log "slot N: not ready, dropping shutter"; vTaskDelete(NULL)
  2. xSemaphoreTake(ctx->client_mutex, pdMS_TO_TICKS(1500 ms))
       on timeout -> log "slot N: shutter lock timeout"; vTaskDelete(NULL)
  3. esp_http_client_set_url(ctx->client, action == SHUTTER_START ? SHUTTER_START_URL : SHUTTER_STOP_URL)
     err = esp_http_client_perform(ctx->client)
     status = esp_http_client_get_status_code(ctx->client)
  4. xSemaphoreGive(ctx->client_mutex)
  5. if (status == 401) handle_401(slot)
     else if (err != ESP_OK || status >= 400) log warning
     else reset ctx->consecutive_401s
  6. vTaskDelete(NULL)
```

The shutter task does not update `ctx->recording_status` — that is left to the 5 s status poll. The mismatch-correction grace period in section 13.4 handles the "command in flight" case.

### 16.7 Periodic Work

A single `esp_timer` (5 s periodic) fires and posts `CMD_POLL_ALL` to the work queue:

```
handle_poll_all()
  for each slot where ctx->http_ready == true:
    xSemaphoreTake(ctx->client_mutex, ...)
    -> GET /gopro/camera/state
        success (200): parse and update ctx->recording_status; reset ctx->consecutive_401s
        401: handle_401(slot)
        other error: log, leave recording_status unchanged
    xSemaphoreGive(ctx->client_mutex)
```

Camera sleep prevention is handled by the BLE 0x5B keepalive (sent every 3 s by `open_gopro_ble`); no HTTP keepalive endpoint is called. The 5 s state poll keeps the TLS session warm as a side effect.

### 16.8 Re-provision Trigger

```c
#define COHN_REPROVISION_THRESHOLD  3

static void handle_401(int slot)
{
    open_gopro_http_ctx_t *ctx = /* ... */;
    ctx->consecutive_401s++;
    ESP_LOGW(TAG, "slot %d: 401 response (%d/%d)",
             slot, ctx->consecutive_401s, COHN_REPROVISION_THRESHOLD);

    if (ctx->consecutive_401s >= COHN_REPROVISION_THRESHOLD) {
        ctx->http_ready = false;
        ctx->consecutive_401s = 0;
        ESP_LOGW(TAG, "slot %d: threshold reached — triggering COHN re-provisioning", slot);
        open_gopro_ble_reprovision(slot);
    }
}
```

`http_ready` is only cleared when the threshold is reached — not on the first 401. A single transient 401 does not interrupt recording commands. The threshold of 3 is chosen to ride out a single transient auth glitch while still detecting genuinely stale credentials within ~15 s at 5 s polls.

### 16.9 Disconnect Cleanup

When `CMD_CAMERA_DISCONNECTED(slot)` is dequeued:

1. Acquire `ctx->client_mutex` (drains any in-flight shutter task)
2. `esp_http_client_cleanup(ctx->client)` — closes connection, frees TLS context
3. `ctx->client = NULL`
4. `ctx->http_ready = false`
5. `ctx->consecutive_401s = 0`
6. `ctx->recording_status = CAMERA_RECORDING_UNKNOWN`
7. Release `ctx->client_mutex`

`teardown(ctx)` (vtable, called by `camera_manager_remove_slot()`) does the same cleanup plus deletes `ctx->client_mutex` and frees the context itself.

### 16.10 Public API

```c
void open_gopro_http_init(void);
void open_gopro_http_on_camera_ready(int slot);
void open_gopro_http_on_ip_updated(int slot, uint32_t new_ip);
void open_gopro_http_on_camera_disconnected(int slot);
void open_gopro_http_on_camera_disconnected_by_mac(const uint8_t mac[6]);
```

### 16.11 Source File Layout

| File | Contents |
|---|---|
| `include/open_gopro_http_spec.h` | All HTTPS endpoint paths, query parameter names, JSON field names, and spec-URL comments. |
| `driver.c` | `open_gopro_http_init()`, vtable registration, `create_driver_ctx()`, public API entry points, vtable `teardown()` |
| `shutter.c` | `shutter_oneshot_task()` body |
| `work.c` | Work task, work queue, `handle_camera_ready()`, `handle_poll_all()`, `handle_ip_updated()`, `handle_disconnect()` |
| `auth.c` | `handle_401()`, re-provision threshold logic, credential refresh |


---

## 17. gopro_wifi_rc

WiFi Remote Control emulation driver for older GoPro cameras (Hero4 and similar). Implements the `camera_driver_t` vtable using a mix of UDP (keepalive) and HTTP (shutter, status, date/time). Camera slot persistence is owned by `camera_manager`; MAC OUI spoofing is owned by `wifi_manager`; WoL is used as the primary reconnect mechanism for cameras that have gone to sleep while still associated to the SoftAP.

### 17.1 Responsibilities

| Responsibility | Notes |
|---|---|
| Implement `camera_driver_t` vtable | `start_recording`, `stop_recording`, `get_recording_status` |
| Station lifecycle | React to L2 associate / DHCP / disassociate events from `wifi_manager` |
| WoL on associate without DHCP | Send magic packet x5; retry every 2 s if camera silent for > 10 s |
| UDP keepalive | `_GPHD_:0:0:2:0.000000\n` unicast to each ready camera, every 3 s |
| HTTP status poll | `GET /gp/gpControl/status` every 5 s; JSON parsed for recording state and camera name |
| HTTP shutter | `GET /gp/gpControl/command/shutter?p=1/0` per camera |
| HTTP date/time | Raw HTTP/1.0 request on GPS lock; Hero4 does not accept standard HTTP/1.1 for command endpoints |
| Discovery exposure | Unknown MACs on SoftAP exposed via `/api/rc/discovered` for manual Add flow |

### 17.2 Protocol Overview

| Transport | Port | Direction | Purpose |
|---|---|---|---|
| UDP unicast | TX: 8484 | ESP32 -> camera | Keepalive only |
| UDP receive | RX: 8383 | camera -> ESP32 | Keepalive ACK only |
| UDP broadcast | TX: 9 | ESP32 -> 255.255.255.255 | WoL magic packet |
| HTTP plain (port 80) | TCP | ESP32 -> camera | Shutter start/stop, status poll, date/time set, probe |

UDP is used exclusively for keepalive and WoL. All status polling and command dispatch is HTTP. TLS is not used — Hero4 cameras have no HTTPS capability on their RC interface. HTTP/1.0 is required for command endpoints; Hero4 returns HTTP 500 to standard HTTP/1.1 requests with extra headers.

### 17.3 Per-Camera Driver Context (`gopro_wifi_rc_ctx_t`)

```c
typedef struct {
    int                       slot;
    uint32_t                  last_ip;

    camera_recording_status_t recording_status;
    bool                      wifi_ready;

    TickType_t                last_keepalive_ack;
    esp_timer_handle_t        keepalive_timer;     /* 3 s periodic */

    esp_timer_handle_t        wol_retry_timer;     /* 2 s periodic */
} gopro_wifi_rc_ctx_t;
```

### 17.4 Threading Model

**Shutter task** (`gopro_rc_shutter`, priority 7): Processes `CMD_SHUTTER_START` and `CMD_SHUTTER_STOP` only. Makes one HTTP call per camera in sequence. Higher priority ensures shutter is never delayed by a status poll or probe.

**Work task** (`gopro_rc_work`, priority 5): Processes all other commands — station connect, DHCP, disconnect, WoL, probe, HTTP status poll, date/time sync. Owns the UDP socket, UDP RX loop, and HTTP client handles for non-shutter calls.

```
wifi_manager callbacks
  -> on_station_associated(mac)      -> work queue: CMD_STATION_ASSOCIATED
  -> on_station_dhcp(mac, ip)        -> work queue: CMD_STATION_DHCP
  -> on_station_disassociated(mac)   -> work queue: CMD_STATION_DISCONNECTED

esp_timer (keepalive, 3 s)    -> work queue: CMD_KEEPALIVE_TICK(slot)
esp_timer (WoL retry, 2 s)    -> work queue: CMD_WOL_RETRY(slot)
esp_timer (status poll, 5 s)  -> work queue: CMD_STATUS_POLL_ALL

camera_driver_t vtable
  -> start_recording(ctx)       -> shutter queue: CMD_SHUTTER_START
  -> stop_recording(ctx)        -> shutter queue: CMD_SHUTTER_STOP
  -> get_recording_status(ctx)  -> read ctx->recording_status directly (no post)
```

### 17.5 Connection Flow

**Rule 1: ignore unknown MACs.** If `camera_manager_find_by_addr(mac)` returns no slot, the MAC is ignored entirely.

**Rule 2: ignore non-RC slots.** After looking up the slot by MAC, check `gopro_model_uses_rc_emulation(slot->model)`. If false, return immediately.

```c
static int find_managed_slot(const uint8_t mac[6])
{
    int slot = camera_manager_find_by_addr(mac);
    if (slot < 0) return -1;
    camera_model_t model = camera_manager_get_model(slot);
    if (!gopro_model_uses_rc_emulation(model)) return -1;
    return slot;
}
```

**Station associates with DHCP lease** — camera is awake and active:

```
handle_station_dhcp(mac, ip)
  -> slot = find_managed_slot(mac)
  -> if slot < 0: return
  -> ctx->last_ip = ip
    camera_manager_save_slot(slot)
    arm ctx->keepalive_timer (3 s periodic)
    post CMD_PROBE(slot) to work queue
```

**Station associates without DHCP lease** — may be asleep or using a cached IP:

```
handle_station_associated(mac)
  -> slot = find_managed_slot(mac)
  -> if slot < 0: return
  -> ip = camera_manager_get_last_ip(slot)
    if ip == 0:
      log warning; return   /* CMD_STATION_DHCP will follow if camera wakes */
    send WoL magic packet x5 (broadcast to 255.255.255.255:9, targeting mac)
    arm ctx->keepalive_timer (3 s periodic)
```

**Probe**:

```
handle_probe(slot)
  -> HTTP GET /gp/gpControl/status (timeout 5 s, up to 3 attempts, 2 s between)
      success: parse camera name, update camera_manager_set_name(slot, name)
               ctx->wifi_ready = true
               camera_manager_on_wifi_connected(slot, ctx->last_ip)
               send date/time (best-effort)
      all attempts fail: log warning; ctx->wifi_ready stays false
```

### 17.6 WoL and Reconnect Logic

```
handle_keepalive_tick(slot)
  -> send UDP keepalive to ctx->last_ip
  -> if (xTaskGetTickCount() - ctx->last_keepalive_ack) > pdMS_TO_TICKS(10000):
        if wol_retry_timer not armed: arm wol_retry_timer (2 s periodic)
  -> else:
        if wol_retry_timer armed: disarm it
        if !ctx->wifi_ready: post CMD_PROBE

handle_wol_retry(slot)
  -> send WoL magic packet x5 (broadcast)
  -> send UDP keepalive (unicast to last_ip)

on_keepalive_ack(src_ip)
  -> find slot by src_ip
  -> ctx->last_keepalive_ack = xTaskGetTickCount()
  -> if !ctx->wifi_ready: post CMD_PROBE(slot)
```

**WoL packet**: 102-byte magic packet: `FF FF FF FF FF FF` + target MAC x16. Broadcast to `255.255.255.255:9`. Sent in a burst of 5.

**WoL retry termination**: The WoL retry timer continues until either a keepalive ACK arrives (disarms the timer) or the camera disassociates from the SoftAP. There is intentionally no max-retry counter — the SoftAP's 60 s inactive-time (section 11.2) evicts the silent station, which disarms the timer in the disconnect handler. WoL retry is naturally bounded to <= 60 s.

### 17.7 Shutter Commands

Handled by the high-priority shutter task:

```
handle_shutter(bool start)
  -> for each slot where ctx->wifi_ready == true:
        HTTP GET /gp/gpControl/command/shutter?p=1   (or p=0)
        timeout: 2 s
        non-200: log warning
```

Sequential dispatch across 4 cameras is expected to complete in < 200 ms total. If measured latency is unacceptable, this is the rollback point to UDP broadcast.

### 17.8 Periodic Work

**Keepalive** (3 s `esp_timer`): UDP unicast `_GPHD_:0:0:2:0.000000\n` to each slot's `last_ip`. This is the liveness signal that drives WoL retry.

**Status poll** (5 s `esp_timer`): Independent timer. On each tick, `GET /gp/gpControl/status` for every slot where `ctx->wifi_ready == true`. The JSON response is parsed for recording state (field `"8"` inside `"status"`) and the camera name (field `"30"`).

**UDP RX loop**: Dedicated lightweight task. Parses only keepalive ACKs (`buf[0] == 0x5F`). All other UDP traffic is logged and discarded.

### 17.9 Disconnect

On `CMD_STATION_DISCONNECTED(mac)`:

1. Disarm `keepalive_timer` and `wol_retry_timer`
2. `ctx->wifi_ready = false`
3. `ctx->recording_status = CAMERA_RECORDING_UNKNOWN`
4. `camera_manager_on_wifi_disconnected(slot)`

### 17.10 NVS Ownership

`gopro_wifi_rc` owns no NVS namespace. All persistence goes through `camera_manager` — MAC, name, model, and `last_ip` all live in `cam_N/camera`.

### 17.11 Public API

```c
void gopro_wifi_rc_init(void);

void gopro_wifi_rc_on_station_associated(const uint8_t mac[6]);
void gopro_wifi_rc_on_station_dhcp(const uint8_t mac[6], uint32_t ip);
void gopro_wifi_rc_on_station_disassociated(const uint8_t mac[6]);

void gopro_wifi_rc_add_camera(const uint8_t mac[6], uint32_t ip);
void gopro_wifi_rc_remove_camera(int slot);
bool gopro_wifi_rc_is_managed_slot(int slot);
bool gopro_wifi_rc_is_managed_mac(const uint8_t mac[6]);

void gopro_wifi_rc_sync_time_all(void);
```

### 17.12 Source File Layout

| File | URL path / area | Contents |
|---|---|---|
| `include/gopro_wifi_rc_spec.h` | All paths | UDP payload constants, HTTP endpoint paths, JSON field names, port numbers. |
| `driver.c` | — | `gopro_wifi_rc_init()`, vtable registration, `create_driver_ctx()`, public API, global timer init |
| `connection.c` | — | Station lifecycle handlers, probe, WoL send, keepalive/WoL-retry timer arming |
| `command.c` | `/gp/gpControl/command/` | Shutter task; date/time set (raw HTTP/1.0 socket) |
| `status.c` | `/gp/gpControl/status` | 5 s status poll timer, `handle_status_poll_all()`, JSON parsing |
| `settings.c` | `/gp/gpControl/settings/` | Placeholder — settings sub-commands not yet implemented. |
| `udp.c` | UDP (ports 8484 / 8383 / 9) | UDP socket init, keepalive send, WoL magic packet send, UDP RX loop, keepalive ACK parsing |


---

## 18. Reconnect Logic

### 18.1 COHN Cameras — BLE Passive Scan

Reconnection is driven by a passive BLE scan managed by `ble_core` and directed by `camera_manager`.

**Scan lifecycle:**

- On boot, `camera_manager` calls `ble_core_start_reconnect_scan(addrs, count)` with the MAC addresses of all configured-but-disconnected COHN cameras.
- When a target is found, `ble_core` fires a callback. `camera_manager` initiates a normal connection.
- If a connection succeeds, `camera_manager` reassesses: if all configured cameras are now connected, it calls `ble_core_stop_scan()`. Otherwise the scan continues.
- If a connection attempt does not complete within **20 seconds**, the camera is returned to `CAM_BLE_NONE` / `WIFI_CAM_NONE` and the scan resumes automatically.
- On mid-session disconnect (camera drops an established connection), `camera_manager` waits **2 seconds** before calling `ble_core_start_reconnect_scan()`.

**Serial reconnection:** BLE allows only one connection attempt at a time (section 12.4). With multiple COHN cameras configured, reconnection is serialised: camera A connects and completes its GATT setup, then the background scan resumes for camera B. In the worst case — all cameras offline simultaneously — the last camera begins reconnecting only after all preceding cameras have finished. In this application (race vehicle, cameras powered together) this is acceptable: once the car is moving, cameras are expected to stay connected. Serial reconnection on boot-up is a one-time cost.

**Whitelist:** The scan should use the NimBLE whitelist (`BLE_HCI_SCAN_FILT_USE_WL`) to drop non-target advertisements at the controller level. If the whitelist proves unreliable, the fallback is software filtering inside the callback. Both approaches produce identical behaviour from `camera_manager`'s perspective.

**State during a connection attempt:** While a connection is in progress, `ble_status` is set to `CAM_BLE_CONNECTING`. If the attempt times out or the GAP event returns an error, `ble_status` returns to `CAM_BLE_NONE` and the scan resumes.

### 18.2 RC-Emulation Cameras — Fully Passive

No active scanning. The SoftAP handles discovery by virtue of the camera connecting to it. `gopro_wifi_rc` ignores all unknown MACs — only cameras already registered in `camera_manager` receive any autonomous action.

**Station associates with a DHCP lease:** The DHCP request proves the camera is awake. `gopro_wifi_rc` records `last_ip`, persists it via `camera_manager_save_slot()`, arms the 3 s UDP keepalive timer, and probes the camera via HTTP. On successful probe the slot transitions to `WIFI_CAM_READY`.

**Station associates without a DHCP lease (cached IP):** The camera may be asleep or reusing a static IP. `gopro_wifi_rc` checks `last_ip` from the camera's NVS record:
- `last_ip` known: send WoL magic packet x5, arm the 3 s keepalive timer. The first keepalive ACK triggers an HTTP probe -> `WIFI_CAM_READY`.
- `last_ip` unknown (never connected before): log warning, take no further action. If the camera wakes and requests a DHCP lease, `CMD_STATION_DHCP` fires and the normal flow resumes.

**Keepalive silence > 10 s (WoL retry):** If no keepalive ACK arrives within 10 s of the timer being armed, a 2 s WoL-retry timer fires repeatedly — sending WoL x5 followed by a keepalive each cycle — until an ACK is received.

**Station disassociates:** Slot returns to `WIFI_CAM_NONE` immediately. Keepalive and WoL-retry timers are disarmed. No further traffic is sent until the camera re-associates.

---

## 19. Web UI and Data Partition

### 19.1 Storage

The project's existing partition table already reserves a 3MB LittleFS partition (`storage`). Web UI source files live in `web_ui/` at the project root; the LittleFS image (`build/storage.bin`) is generated automatically during `idf.py build` and flashed as part of `idf.py flash`.

Benefits:
- Web UI can be re-flashed independently from firmware (`idf.py storage-flash`).
- Browser can cache JS and CSS separately from the HTML shell.
- Pre-compressed assets reduce transfer size significantly on the SoftAP link.

### 19.2 File layout

Source files (checked in to `web_ui/`):
```
web_ui/
  index.html        HTML shell
  style.css         Stylesheet
  app.js            Application logic
  compress.py       Build helper — called by CMake to copy + gzip assets
```

On-device layout (`/www/` on LittleFS):
```
/www/
  index.html        Copied as-is
  style.css         Plain copy
  style.css.gz      Pre-compressed (gzip -9); served when Accept-Encoding: gzip present
  app.js            Plain copy
  app.js.gz         Pre-compressed; served when Accept-Encoding: gzip present
```

`compress.py` is invoked by a CMake custom target (`web_ui_stage`) defined in the root `CMakeLists.txt`. `littlefs_create_partition_image()` (from `joltwallet__littlefs`) then builds `build/storage.bin` from the staged output and wires it into `idf.py flash` via `FLASH_IN_PROJECT`.

### 19.3 Serving compressed assets

The HTTP server checks `Accept-Encoding: gzip` in the request headers (sent by every modern browser and iOS Safari). If present, it serves the `.gz` file with:

```
Content-Encoding: gzip
Content-Type: application/javascript   (or text/css)
Content-Length: <compressed size>
Cache-Control: max-age=3600
```

This also resolves the observed iOS loading issues. iOS Safari requires an accurate `Content-Length` header and handles chunked transfer encoding poorly from embedded HTTP servers. Serving a known-size pre-compressed file with explicit headers satisfies both requirements.

### 19.4 Web UI source location

Web UI source files live in `web_ui/` at the project root (not inside any component). LittleFS image generation is wired into the root `CMakeLists.txt` so it runs as part of every build. `wifi_manager` and `http_server` have no build-time dependency on the web UI source.

---

## 20. http_server

The HTTP server is the topmost component in the dependency graph. It owns the `esp_httpd` instance, serves web assets from LittleFS, and implements all `/api/` endpoint handlers. It has no logic of its own beyond routing — all decisions are delegated to the component being called.

### 20.1 Responsibilities

| Responsibility | Notes |
|---|---|
| Serve web UI assets | `index.html`, `app.js.gz`, `style.css.gz` from LittleFS `/www/` |
| Handle all `/api/` endpoints | Route to the appropriate component; build JSON responses |
| No business logic | Handlers read arguments, call one component API, write response |

`http_server` depends on every other component. No other component depends on it.

### 20.2 Task Model

`esp_httpd` runs on its own internal task. API handlers are called directly on that task — no additional queue or dispatch task is needed. This is safe because read operations are non-blocking RAM reads, state-setting calls are flag writes, and BLE/WiFi RC operations already return immediately by posting to their own component queues internally.

`esp_httpd` stack size and max open sockets are no longer Kconfig symbols in IDF v6 and must be set programmatically in `http_server_init()`:

```c
httpd_config_t config = HTTPD_DEFAULT_CONFIG();
config.stack_size      = 8192;   /* default 4 KB is tight when building JSON for all camera slots */
config.max_open_sockets = 16;    /* matches V1; handles concurrent browser + background polls */
httpd_start(&server, &config);
```

### 20.3 Serving Web Assets

On `GET /` and `GET /index.html`: serve `/www/index.html` from LittleFS.

On `GET /app.js` and `GET /style.css`: check `Accept-Encoding` header. If `gzip` is present, serve the `.gz` file with explicit `Content-Encoding: gzip` and `Content-Length`. This satisfies iOS Safari's requirement for accurate content length on compressed responses (see section 19.3).

All asset responses include `Cache-Control: max-age=3600`.

### 20.4 API Handlers

All handlers follow the same structure: parse request -> call one component API -> write JSON response. The full endpoint contract lives in **`web_ui_spec.md`**, which is the canonical reference for the HTTP API contract. This section only maps endpoints to the component each handler calls into.

| Endpoint | Component called |
|---|---|
| `GET /api/logging-state` | `can_manager_get_logging_state()` |
| `GET /api/utc` | `can_manager_get_utc_ms()` |
| `GET /api/auto-control` | `camera_manager_get_auto_control()` |
| `POST /api/auto-control` | `camera_manager_set_auto_control()` |
| `GET /api/paired-cameras` | `camera_manager_get_all_slot_info()` |
| `POST /api/shutter` | `camera_manager_set_desired_recording_all()` or `_slot()` |
| `POST /api/remove-camera` | `camera_manager_remove_slot()` (section 20.5) |
| `POST /api/reorder-cameras` | `camera_manager_reorder_slots()` (section 20.6) |
| `GET /api/cameras` | `open_gopro_ble_get_discovered()` |
| `POST /api/scan` | `open_gopro_ble_start_discovery()` |
| `POST /api/scan-cancel` | `open_gopro_ble_stop_discovery()` |
| `POST /api/pair` | `open_gopro_ble_connect_by_addr()` |
| `GET /api/rc/discovered` | `wifi_manager_get_connected_stations()` filtered by `gopro_wifi_rc_is_managed_mac()` |
| `POST /api/rc/add` | `gopro_wifi_rc_add_camera()` |
| `GET /api/settings/timezone` | `can_manager_get_tz_offset_hours()` |
| `POST /api/settings/timezone` | `can_manager_set_tz_offset()` |
| `POST /api/settings/datetime` | `can_manager_set_manual_utc_ms()` -> fires UTC-acquired path |
| `POST /api/reboot` | `esp_restart()` |
| `POST /api/factory-reset` | NVS erase all -> `esp_restart()` |

### 20.5 Unified Camera Remove with Slot Compaction

`camera_manager_remove_slot(slot)` is the single entry point for all camera removal. After removing the target slot, all higher-indexed slots are compacted down by one position so that the slot array is always contiguous.

**Remove and compact sequence:**

1. Call `driver->teardown(ctx)` if non-NULL — allows manufacturer components to clean up their own NVS keys and free per-slot resources.
2. Call `ble_core_remove_bond(mac)` for COHN cameras (BLE bond must be cleared so the camera does not auto-reconnect).
3. Erase `cam_N/camera` (and `cam_N/gopro_cohn` if applicable) from NVS.
4. For each slot index i from the removed slot up to (MAX_SLOTS - 2):
   - Copy RAM slot i+1 -> RAM slot i (struct copy).
   - Call `driver->update_slot_index(ctx, i)` so the driver context's cached slot number reflects the new position.
   - Write `cam_i/camera` to NVS from the new RAM slot i content.
   - If `cam_i/gopro_cohn` existed under i+1, copy it to the new key. Erase the old key.
5. Zero the highest RAM slot (now a duplicate after the shift).
6. Erase `cam_(last)/camera` from NVS.

**`update_slot_index` vtable entry** (added to `camera_driver_t`):

```c
struct camera_driver {
    esp_err_t (*start_recording)(void *ctx);
    esp_err_t (*stop_recording)(void *ctx);
    camera_recording_status_t (*get_recording_status)(void *ctx);
    void      (*teardown)(void *ctx);                        /* nullable */
    void      (*update_slot_index)(void *ctx, int new_slot); /* nullable */
};
```

`open_gopro_http` implements `teardown` and `update_slot_index`. `open_gopro_ble` implements `teardown` (erase `cam_N/gopro_cohn`) and `update_slot_index`. `gopro_wifi_rc` implements `update_slot_index`; `teardown` may be NULL.

**CAN `0x601` output changes immediately** after compaction. If the RaceCapture configuration maps CAN byte positions to camera identities, the operator must update RaceCapture to match after removing a camera.

### 20.6 Manual Slot Reordering

The web UI allows the operator to reorder cameras. Reordering requires all affected cameras to be disconnected first (the API returns an error if any slot in the reorder set is currently `WIFI_CAM_READY` or `CAM_BLE_CONNECTED`).

```c
esp_err_t camera_manager_reorder_slots(const int *new_order, int count);
```

```
POST /api/reorder-cameras
Body: { "order": [2, 0, 3, 1] }
  — new_order[0]=2 means "put the camera currently in slot 2 into slot 0"
Response: 200 OK on success, 409 Conflict if any camera is currently connected
```

**Implementation:** `camera_manager_reorder_slots` performs the permutation in RAM, writes all affected `cam_N/camera` records to NVS in the new order, copies any `cam_N/gopro_cohn` keys to new positions (erasing the old keys), and calls `update_slot_index` on each driver context for affected slots.

**Note on RaceCapture:** reordering changes which CAN byte corresponds to which physical camera. The operator must update the RaceCapture channel mapping after reordering.

### 20.7 Source File Layout

| File | Contents |
|---|---|
| `driver.c` | `http_server_init()`, `esp_httpd` start, LittleFS mount, static asset handlers |
| `api_cameras.c` | `/api/paired-cameras`, `/api/shutter`, `/api/remove-camera`, `/api/reorder-cameras`, `/api/scan`, `/api/scan-cancel`, `/api/pair`, `/api/cameras` |
| `api_rc.c` | `/api/rc/discovered`, `/api/rc/add` |
| `api_settings.c` | `/api/settings/timezone`, `/api/settings/datetime` |
| `api_system.c` | `/api/logging-state`, `/api/utc`, `/api/auto-control`, `/api/reboot`, `/api/factory-reset` |


---

## 21. Startup Sequence

### 21.1 Design Constraints

Two constraints shape the init order:

**Networking last.** `wifi_manager_init()` is the call that raises the SoftAP and makes it visible. An RC-emulation camera could associate the moment the AP is visible. If `gopro_wifi_rc`'s station callbacks are not yet registered when that happens, the event is lost. Similarly, `ble_core_init()` starts the NimBLE host task; the `on_sync` callback fires asynchronously and immediately begins scanning. All BLE and WiFi RC callbacks must be wired before their respective stacks are started.

**Drivers before slots.** `camera_manager_init()` loads slot records from NVS but cannot assign drivers until the drivers have registered themselves — see section 21.4 for the registration mechanism. By the time all driver inits are complete, all loaded slots whose model is recognised by some registered driver have a live driver pointer and context. Slots whose model matches no driver are left in an unconfigured state and logged at WARN.

### 21.2 Init Order

```
app_main()
  |
  +- nvs_flash_init()
  |    NVS is the prerequisite for every component that reads or writes
  |    persistent state. Must be first.
  |
  +- camera_manager_init()
  |    Loads all cam_N/camera records from NVS into RAM slots.
  |    All slots start with ble_status=CAM_BLE_NONE, wifi_status=WIFI_CAM_NONE.
  |    No drivers assigned yet.
  |
  +- open_gopro_http_init()
  |    Calls camera_manager_register_driver(&s_cohn_https_driver,
  |                                          gopro_model_uses_cohn,
  |                                          open_gopro_http_create_ctx).
  |    camera_manager iterates loaded slots, assigns this driver to all
  |    matching COHN slots and creates their per-slot contexts.
  |    Starts work task. No network traffic yet.
  |
  +- gopro_wifi_rc_init()
  |    Calls camera_manager_register_driver(&s_rc_driver,
  |                                          gopro_model_uses_rc_emulation,
  |                                          gopro_wifi_rc_create_ctx).
  |    Starts shutter task + work task. Opens UDP socket.
  |    Starts global keepalive and status-poll timers
  |    (timers iterate wifi_ready slots — none yet, so they fire and return).
  |
  +- open_gopro_ble_init()
  |    Registers its callback struct with ble_core (stores pointers; ble_core
  |    not yet started so no callbacks fire here).
  |    Calls ble_core_purge_unknown_bonds(known_macs, count) — walks the NimBLE
  |    peer-security store and deletes any bonds not in camera_manager's configured
  |    COHN slot list. Safe here because the NimBLE host task has not started yet;
  |    no connections are possible, so there is no race between the purge and an
  |    incoming connection. This call must not be made after ble_core_init().
  |    Allocates internal state. Does not start the keepalive timer yet —
  |    that is armed per-slot when a camera reaches BLE ready.
  |
  +- ble_core_init()
  |    Starts the NimBLE host task.
  |    on_sync fires asynchronously once NimBLE is ready:
  |      -> start_scan_if_needed() — begins passive scan if any COHN slots
  |        are configured but disconnected.
  |    No explicit call from main() required after this point for BLE.
  |
  +- can_manager_init(&s_can_callbacks)
  |    Loads tz_offset from NVS.
  |    Starts TWAI driver and RX task.
  |    Registers s_can_callbacks (defined in main.c — see section 21.3).
  |
  +- wifi_manager_init(&s_wifi_callbacks)
  |    Registers s_wifi_callbacks (defined in main.c — see section 21.3).
  |    Spoofs SoftAP MAC to d8:96:85:XX:XX:XX.
  |    Raises SoftAP — SSID now visible to cameras.
  |    Starts DHCP server.
  |    *** From this point RC-emulation cameras may associate. ***
  |
  +- http_server_init()
       Mounts LittleFS partition (/www/).
       Starts esp_httpd instance.
       Registers all URI handlers.
       Web UI now accessible.
```

### 21.3 Callback Wiring

All cross-component wiring lives in `main.c`. Components register callbacks through their init functions; `main.c` defines the handler bodies that call into the appropriate components.

#### CAN callbacks (`s_can_callbacks`)

```c
static void on_logging_state_changed(can_logging_state_t state)
{
    /* Fires on every received 0x600 frame (section 14.2) — idempotent on the
     * camera_manager side since set_desired_recording_all() is itself idempotent. */
    if (state == LOGGING_STATE_LOGGING || state == LOGGING_STATE_NOT_LOGGING) {
        bool recording = (state == LOGGING_STATE_LOGGING);
        camera_manager_set_desired_recording_all(recording);
    }
    /* LOGGING_STATE_UNKNOWN is reflected in the web UI but does not modify
     * desired_recording — see section 13.2. */
}

static void on_utc_acquired(void)
{
    open_gopro_ble_sync_time_all();
    gopro_wifi_rc_sync_time_all();
}

static const can_callbacks_t s_can_callbacks = {
    .on_logging_state_changed = on_logging_state_changed,
    .on_utc_acquired          = on_utc_acquired,
};
```

`on_utc_acquired` fires exactly once — on the first valid GPS timestamp from RaceCapture. Both camera types are notified simultaneously. `open_gopro_ble_sync_time_all()` also unblocks any slots with `cohn_pending_utc` set (section 15.5).

#### WiFi callbacks (`s_wifi_callbacks`)

```c
static void on_station_associated(const uint8_t mac[6])
{
    gopro_wifi_rc_on_station_associated(mac);
    /* COHN cameras: open_gopro_ble tracks the SoftAP join internally
     * via RequestGetCOHNStatus polling — no callback needed here. */
}

static void on_station_dhcp(const uint8_t mac[6], uint32_t ip)
{
    camera_manager_on_station_ip(mac, ip);   /* updates last_ip for any matching slot */
    gopro_wifi_rc_on_station_dhcp(mac, ip);
}

static void on_station_disassociated(const uint8_t mac[6])
{
    gopro_wifi_rc_on_station_disassociated(mac);           /* RC-emulation path    */
    open_gopro_http_on_camera_disconnected_by_mac(mac);    /* COHN path            */
    /* Each handler applies its own model-type guard — only the owning driver acts. */
}

static const wifi_callbacks_t s_wifi_callbacks = {
    .on_station_associated    = on_station_associated,
    .on_station_dhcp          = on_station_dhcp,
    .on_station_disassociated = on_station_disassociated,
};
```

`on_station_disassociated` notifies both components because a camera leaving the SoftAP is relevant to both: `gopro_wifi_rc` needs to tear down its keepalive timers; `open_gopro_http` needs to mark the slot not-ready and stop sending HTTPS commands. Each component's handler applies its own model-type guard (section 17.5) so that they only act on slots they own.

#### BLE callbacks

Registered by `open_gopro_ble_init()` directly via `ble_core_register_callbacks()` — no wiring required in `main.c`. `open_gopro_ble` owns the full BLE callback set.

### 21.4 Driver Registration Mechanism

`camera_manager` does not depend on any driver component. Drivers know about `camera_manager`, not the other way around — that is the whole point of the vtable in section 13.5. At boot, `camera_manager` has no way to construct a driver itself; the drivers must announce themselves.

**The registration call:**

```c
typedef bool   (*camera_model_match_fn)(camera_model_t model);
typedef void  *(*camera_ctx_create_fn)(int slot);

esp_err_t camera_manager_register_driver(
    const camera_driver_t *driver,
    camera_model_match_fn   matches,
    camera_ctx_create_fn    create_ctx);
```

Each driver component calls this from its `_init()` function. For example:

```c
/* in open_gopro_http_init() */
camera_manager_register_driver(&s_cohn_https_driver,
                                gopro_model_uses_cohn,
                                open_gopro_http_create_ctx);
```

**What `camera_manager` does on registration:**

1. Stores the registration in a small fixed-size table.
2. Iterates all currently-loaded slots. For each slot whose `model` returns `true` from the registered `matches()`:
   - Calls `create_ctx(slot)` and stores the returned pointer in `slot->driver_ctx`.
   - Stores the `driver` pointer in `slot->driver`.
3. Logs a WARN for any slot that ends up with no matching driver after all expected drivers have registered.

**Why predicate functions instead of a `camera_driver_type_t` enum?** A predicate keeps GoPro-specific knowledge out of `camera_manager`. With an enum, `camera_manager` would need to know "model X corresponds to driver type Y" — which is exactly the GoPro-specific decision the architecture is trying to keep out. With a predicate, each driver answers "is this slot mine?" using its own logic, and `camera_manager` never has to map model values.

**Boot ordering.** All driver `_init()` calls in section 21.2 happen before `wifi_manager_init()` and `ble_core_init()` raise their stacks. Driver assignment is guaranteed complete before any driver method can be called.

### 21.5 Post-Boot Steady State

After `http_server_init()` returns, the system is fully operational:

- NimBLE is scanning passively for configured COHN cameras
- SoftAP is visible; RC-emulation cameras and COHN cameras can join
- CAN bus is listening for `0x600` logging commands and `0x602` UTC frames
- `0x601` camera-state frames are being transmitted at 5 Hz regardless of camera connection state
- Web UI is accessible at `http://10.71.79.1/`

No component requires any further call from `main()`. All subsequent activity is event-driven.

---

## 22. Logging Strategy

ESP-IDF's `esp_log` macros are the baseline. The conventions below are not enforced by tooling — they are what every component in V2 should follow.

### 22.1 Tags

```c
static const char *TAG = "camera_manager";
```

One TAG per `.c` file (or one per logical area within a component). Tag string equals component name, lowercase, no spaces. Sub-components (e.g. `gopro_ble_pairing`) can use a more specific tag if log volume warrants it.

### 22.2 Per-slot prefix

Every per-slot log line includes `slot %d:` early in the format string, for grep-ability:

```c
ESP_LOGI(TAG, "slot %d: HTTPS ready (ip=%lu)", slot, ip);
ESP_LOGW(TAG, "slot %d: shutter lock timeout", slot);
```

### 22.3 Levels

| Macro | Use for |
|---|---|
| `ESP_LOGE` | Unrecoverable for this operation: NVS write failed, vtable null deref avoided, OOM, unrecoverable protocol error |
| `ESP_LOGW` | Transient error, retry in progress, unexpected-but-handled: HTTP non-200, BLE timeout, missing model, single 401 |
| `ESP_LOGI` | Milestone state transitions: slot configured, COHN provisioned, camera ready, station joined, scan started/stopped |
| `ESP_LOGD` | Verbose state-machine traces: each scan event, each poll fire, each timer arm, individual GATT writes |
| `ESP_LOGV` | Packet-level dumps. Off by default. Per-component opt-in when investigating something specific |

### 22.4 Spam control

Anywhere a condition can fire repeatedly (mismatch correction loop, WoL retry, repeated 401s short of the threshold), log only on transition. Pattern:

```c
if (state != ctx->last_logged_state) {
    ESP_LOGW(TAG, "slot %d: transitioned to %s", slot, state_name(state));
    ctx->last_logged_state = state;
}
```

Do not suppress logs with timers — suppress by transition. A log line that fires once per state change is debuggable; a line that fires once every 5 s per slot drowns the console.

### 22.5 Runtime tuning

Per-component log levels can be set at runtime in `app_main()` before component init:

```c
esp_log_level_set("*",                ESP_LOG_INFO);
esp_log_level_set("open_gopro_ble",    ESP_LOG_DEBUG);
esp_log_level_set("ble_core",          ESP_LOG_DEBUG);
```

This survives without rebuild — useful in the field.

### 22.6 Build-time configuration

`CONFIG_LOG_DEFAULT_LEVEL` in sdkconfig sets the compile-time maximum. `CONFIG_LOG_MAXIMUM_LEVEL` sets the absolute ceiling for `esp_log_level_set()` to override. Set the maximum to `VERBOSE` so runtime tuning is unconstrained, and the default to `INFO` so a release build is quiet.

---

## 23. Test Strategy

V2 testing is **host-side unit tests only**. Hardware testing is performed manually using a real ESP32, real cameras, and a real CAN bus — covered by personal experience and not automated.

This split is deliberate: on-device automated testing for an embedded control system with BLE+WiFi+CAN is high-effort to set up and produces flaky results. The host-side tests are cheap to run and catch the class of bugs that table-driven logic is most prone to.

### 23.1 Host-side unit tests with Unity

ESP-IDF ships with **Unity** (`unity/`) — a small C testing framework. Pure-logic source files that don't include ESP-IDF headers can be compiled and run on the development host using a tiny CMake shim, completely independent of the ESP32 build.

**Targets that benefit most:**

- **`gopro/gopro_model.h` capability helpers.** All pure functions, no dependencies. Table-driven Unity tests covering every defined `camera_model_t` value plus boundary cases (CAMERA_MODEL_UNKNOWN, values just outside the GoPro ranges, hypothetical 1000+ multi-manufacturer values).
- **The mismatch-correction state machine (section 13.4)** — if extracted from the timer callback into a pure step function:

  ```c
  typedef enum {
      MISMATCH_ACTION_NONE,
      MISMATCH_ACTION_START,
      MISMATCH_ACTION_STOP,
  } mismatch_action_t;

  mismatch_action_t mismatch_step(bool desired_recording,
                                   camera_recording_status_t actual,
                                   bool grace_period_active);
  ```

  Then exhaustively unit-test the truth table: 2 x 3 x 2 = 12 input combinations, each with a known-correct output.
- **JSON parsing routines** for `/gopro/camera/state` and `/gp/gpControl/status`. Feed in canned response strings (captured from real cameras), assert parsed `camera_recording_status_t` and camera name.
- **CAN frame parsers** — `0x600`, `0x601`, `0x602` — separated from the TWAI driver so the parser is a pure `parse(uint8_t *bytes, size_t len) -> can_frame_t`. Cover bad payloads, short payloads, year-out-of-range UTC.

### 23.2 Build setup

A subdirectory `tests/host/` with its own `CMakeLists.txt` that compiles only the pure-logic files plus Unity, builds an executable on the host, and runs it via CTest. Files that include `esp_log.h`, `freertos/FreeRTOS.h`, `esp_http_client.h`, `nimble/...`, etc. are out of scope.

The convention to make this work: keep pure-logic functions in files that intentionally avoid ESP-IDF includes — `mismatch.c`, `gopro_model.c`, `can_parse.c` — and wire them up from the platform-aware files that do include ESP-IDF headers.

### 23.3 Recommended starting point

Pick **one** target — `gopro_model.h` capability helpers is the easiest — and get the host-side build running with one passing test. Once that workflow is proven (CMake + CTest + Unity all wired up), expand to the mismatch state machine and JSON parsers. The goal is having the workflow in place; coverage grows organically from there.

### 23.4 Out of scope (covered by manual testing)

The following are explicitly NOT in the unit test plan — they are validated manually on real hardware:

- Multi-camera RF behaviour and BLE/WiFi coexistence under load.
- Camera-side state transitions (record/stop, sleep/wake).
- WoL recovery from real battery-dead conditions.
- iOS / Safari rendering of the web UI.
- CAN bus integration with the RaceCapture device.
- COHN provisioning end-to-end with a real GoPro.

---

## 24. Open Items

The following will be designed in subsequent sessions:
- **Live telemetry:** Battery percentage, storage remaining, and other camera-reported values. Will be polled via HTTPS on the tick timer, cached in the slot, and added to `camera_slot_info_t`.
- **Additional `gopro_model.h` capability helpers:** Further behavioral differences (supported HTTPS endpoints, status poll intervals, etc.) documented as cameras are tested.

### 24.1 Multi-manufacturer hooks (deferred)

Adding a second camera manufacturer is not designed in this revision. The intent is to keep V2 from making decisions that would later be expensive to undo, without prematurely building hooks we don't yet understand. Concretely:

- **`camera_manager` is the boundary.** It already takes drivers via `register_driver()` with predicate functions over `camera_model_t`. A new manufacturer just registers their own driver(s) with their own model-range predicate. No `camera_manager` change.
- **`camera_model_t` reserves 1000+ for non-GoPro values.** New manufacturer ranges go there. GoPro capability helpers in `gopro/gopro_model.h` already gate on the GoPro ranges (section 5.2), so they will not misclassify.
- **`ble_core` is manufacturer-agnostic.** It exposes `on_disc` to whoever registers — currently `open_gopro_ble`. When a second BLE-using manufacturer is added, the routing question (which provisioning component handles a given advertisement) becomes real. **Do not pre-build this.** Today there is one consumer of `on_disc`, and that consumer applies its own UUID filter. When a second manufacturer arrives, the design choice becomes: either add a registration list to `ble_core` (each provisioning component registers a UUID filter, `ble_core` dispatches), or have a thin `ble_dispatch` shim above `ble_core`. Either is straightforward to retrofit. The trap to avoid is *guessing* the routing API now — get it wrong and we'll be stuck with awkward callbacks.
- **`wifi_manager` is manufacturer-agnostic.** The station table is keyed by MAC; each driver looks up its own slots. A second manufacturer just adds another driver listening on the same callback set.
- **`http_server` will need to know.** The pairing flow (`/api/scan`, `/api/pair`) is currently GoPro-specific. When a second manufacturer arrives, this likely splits into per-manufacturer endpoints — the web UI controls the dispatch by which endpoint it calls. No need to design this until the second manufacturer is real.

The corner-paint risk is small as long as we keep the `gopro/` subtree clearly bounded and don't leak GoPro assumptions into `camera_manager`, `ble_core`, `wifi_manager`, or `http_server`.

# camera_manager

Manages up to four camera slots: per-slot state (BLE and WiFi connection status, recording intent), NVS persistence, driver vtable dispatch, and the periodic mismatch-correction loop that keeps each camera's actual recording state aligned with the operator's intent.

`camera_manager` is the hub that infrastructure components (`ble_core`, `wifi_manager`) and protocol drivers (`open_gopro_ble`, `open_gopro_http`, `gopro_wifi_rc`, `can_manager`) all wire into. It has no direct knowledge of GoPro-specific protocols — behavioral branching by model lives in `gopro/gopro_model.h` and is called only by the driver components.

---

## Responsibilities

- Load per-camera slot records (`cam_N/camera`) from NVS on boot.
- Accept driver registrations from protocol components and assign each driver to the slots whose model it owns.
- Track coarse BLE and WiFi connection status for each slot.
- Run a per-slot periodic mismatch-correction timer: compare the driver's cached recording status against the operator's desired state and issue `start_recording()` / `stop_recording()` as needed.
- Persist `last_ip` and other slot fields to NVS on update.
- Provide slot lookup and state query functions for HTTP handlers and `can_manager`.
- Provide `is_known_addr` and `has_disconnected_cameras` predicates for `ble_core`'s scan gate.

---

## Dependencies

```
REQUIRES: bt (ble_addr_t type), nvs_flash, esp_timer, freertos
```

**Precondition:** `nvs_flash_init()` must be called before `camera_manager_init()`.

---

## Source Files

| File | Responsibility |
|------|---------------|
| `include/camera_types.h` | Pure C enums and the `mismatch_step()` declaration — no ESP-IDF includes (host-testable) |
| `include/camera_manager.h` | Full public API: driver vtable, slot info, all functions |
| `camera_manager.c` | Init, NVS load/save, driver registration, slot lifecycle, mismatch timer |
| `mismatch.c` | Pure `mismatch_step()` function — no ESP-IDF includes (host-testable) |

---

## NVS Layout

Each slot occupies namespace `cam_N` (N = 0–3). Only `camera_manager` writes the `camera` key; driver components own their own keys in the same namespace.

```
cam_0/camera   ← camera_nv_record_t (schema version 1)
cam_0/gopro_cohn  ← owned by open_gopro_ble (not touched here)
cam_1/camera
...
```

**Schema version policy:** a blob with a mismatched `version` byte is discarded and the slot is left unconfigured. Re-pairing is required. No automatic migration.

**Save-time validation:** `camera_manager_save_slot()` returns `ESP_ERR_INVALID_ARG` and refuses to write if `model == CAMERA_MODEL_UNKNOWN`.

---

## Driver Registration (§21.4)

Drivers call `camera_manager_register_driver()` from their own `_init()` functions — `camera_manager` never imports a driver header. This keeps the dependency arrows pointing one way:

```
open_gopro_http  ──┐
gopro_wifi_rc    ──┼──► camera_manager
can_manager      ──┘
```

On registration, `camera_manager` immediately iterates all loaded slots and assigns the new driver to any whose model satisfies the `matches()` predicate. Driver assignment for slots loaded after registration (via `camera_manager_register_new()` or `camera_manager_set_model()`) happens at those call sites.

The `requires_ble` flag controls `has_disconnected_cameras()`: RC-emulation cameras never hold a BLE connection, so they must not be counted as "disconnected" for the BLE background scan gate.

---

## Recording State Machine (§13)

Each slot carries two independent pieces of state:

| Field | Source | Meaning |
|-------|--------|---------|
| `desired_recording` | CAN `0x600` frames or web UI | What the operator wants |
| `get_recording_status()` | Driver cache (non-blocking) | What the camera is actually doing |

The mismatch-correction timer fires every **2 s** while a slot is `WIFI_CAM_READY`. On each tick:

```
desired == UNKNOWN               → no-op (boot state, no intent yet)
actual  == UNKNOWN               → no-op (camera state not yet known)
grace_period_active              → no-op (command from last tick still in flight)
desired == START, actual == IDLE  → start_recording(); set grace_period_active
desired == STOP, actual == ACTIVE → stop_recording(); set grace_period_active
```

`grace_period_active` is cleared on the **next** tick regardless of whether the camera's status has updated. This gives each command one full poll cycle before a correction is re-issued.

`mismatch_step()` is a pure function in `mismatch.c` — no side effects, no ESP-IDF headers — and can be unit-tested on the host (§23.2).

---

## Slot Lifecycle

```
camera_manager_init()
  → load NVS records; all BLE/WiFi statuses = NONE

camera_manager_register_new(mac)    [called by open_gopro_ble on first encrypted connection]
  → allocates slot; model = UNKNOWN; no NVS write

camera_manager_set_model(slot, model)
  → assigns matching registered driver; open_gopro_ble then calls camera_manager_save_slot()

camera_manager_on_ble_connected(slot, conn_handle)  → CAM_BLE_CONNECTED
camera_manager_on_ble_ready(slot)                   → CAM_BLE_READY

camera_manager_set_camera_ready(slot, true)          → WIFI_CAM_CONNECTED
  (open_gopro_ble calls this; driver probe follows separately)

camera_manager_on_wifi_connected(slot, ip)           → WIFI_CAM_READY + start mismatch timer
camera_manager_on_wifi_disconnected(slot)            → WIFI_CAM_NONE  + stop mismatch timer

camera_manager_remove_slot(slot)
  → stop timer, teardown driver, erase NVS, compact slot array, notify drivers of new indices
```

---

## Public API

Header: `include/camera_manager.h`  
Pure types: `include/camera_types.h`

### Init

```c
void camera_manager_init(void);
```

Load all `cam_N/camera` NVS records. Must be called before any other function and before any driver `_init()`.

---

### Driver Registration

```c
esp_err_t camera_manager_register_driver(
    const camera_driver_t *driver,
    camera_model_match_fn   matches,
    camera_ctx_create_fn    create_ctx,
    bool                    requires_ble
);
```

Register a driver. `matches(model)` is called for each slot to decide ownership. `create_ctx(slot)` allocates a per-slot driver context. `requires_ble` controls `has_disconnected_cameras()`. Returns `ESP_ERR_NO_MEM` if the registration table (4 entries) is full.

---

### Slot Lookup

```c
int camera_manager_find_by_mac(const uint8_t mac[6]);
```

Linear search of configured slots by raw 6-byte MAC. Returns slot index or `-1`. Used by both WiFi callbacks (station MAC) and BLE callbacks (BLE address bytes).

```c
int camera_manager_register_new(const uint8_t mac[6]);
```

Allocate a new slot for an unknown camera. Does not write NVS. Returns slot index or `-1` if all four slots are occupied.

---

### BLE State Transitions

```c
void camera_manager_on_ble_connected(int slot, uint16_t conn_handle);
void camera_manager_on_ble_ready(int slot);
void camera_manager_on_ble_disconnected_by_handle(uint16_t conn_handle);
```

`on_ble_disconnected_by_handle` locates the slot by handle — call it **after** any per-slot BLE cleanup that needs the handle (e.g. stopping timers) but before the higher layer forgets the handle.

---

### Slot Field Updates

```c
void camera_manager_set_model(int slot, camera_model_t model);
void camera_manager_set_name(int slot, const char *name);
void camera_manager_set_camera_ready(int slot, bool ready);
```

`set_camera_ready(true)` sets `WIFI_CAM_CONNECTED` (IP assigned; driver probe can proceed). It does **not** set `WIFI_CAM_READY` — that is done by the driver after its probe succeeds via `on_wifi_connected()`.

---

### WiFi State

```c
void camera_manager_on_wifi_connected(int slot, uint32_t ip);
void camera_manager_on_wifi_disconnected(int slot);
void camera_manager_on_station_ip(const uint8_t mac[6], uint32_t ip);
```

`on_wifi_connected` sets `WIFI_CAM_READY` and starts the mismatch poll timer.  
`on_wifi_disconnected` stops the timer and resets `wifi_status` to `WIFI_CAM_NONE`.  
`on_station_ip` updates `last_ip` for any configured slot whose MAC matches (called from the wifi_manager DHCP callback).

---

### NVS

```c
esp_err_t camera_manager_save_slot(int slot);
```

Serialize the slot to `cam_N/camera`. Returns `ESP_ERR_INVALID_ARG` if `model == CAMERA_MODEL_UNKNOWN`.

---

### Queries

```c
uint32_t           camera_manager_get_last_ip(int slot);
camera_model_t     camera_manager_get_model(int slot);
int                camera_manager_get_slot_count(void);
esp_err_t          camera_manager_get_slot_info(int slot, camera_slot_info_t *out);
camera_can_state_t camera_manager_get_slot_can_state(int slot);
```

`get_slot_info` copies all display-relevant state into `camera_slot_info_t` (used by HTTP handlers). `is_recording` is derived inline: `wifi_status == READY && driver->get_recording_status() == ACTIVE`.

`get_slot_can_state` translates slot state to the four `CAMERA_CAN_STATE_*` values used in the CAN `0x601` broadcast. Called by `can_manager` when building the payload.

---

### Recording Intent

```c
void camera_manager_set_desired_recording_all(desired_recording_t intent);
void camera_manager_set_desired_recording_slot(int slot, desired_recording_t intent);
bool camera_manager_get_auto_control(void);
void camera_manager_set_auto_control(bool enabled);
```

`set_desired_recording_all` is called by `can_manager` on every received `0x600` frame (idempotent by design). `set_desired_recording_slot` is used by HTTP handlers for manual per-camera control when `auto_control == false`.

Setting `desired_recording` does not immediately issue a command — the mismatch timer drives the correction on its next tick.

---

### Slot Removal

```c
esp_err_t camera_manager_remove_slot(int slot);
```

Stop the mismatch timer, call `driver->teardown()`, erase NVS for the slot, compact the array (slots above shift down by one), update NVS for compacted slots, and notify each driver of its new index via `driver->update_slot_index()`.

**CAN output changes immediately** after compaction — byte positions in the `0x601` frame shift.

---

### ble_core Callbacks

```c
bool camera_manager_is_known_ble_addr(ble_addr_t addr);
bool camera_manager_has_disconnected_cameras(void);
```

Pass these as function pointers when constructing `ble_core_callbacks_t` in `open_gopro_ble_init()`. `is_known_addr` gates auto-reconnect in the background scan handler. `has_disconnected_cameras` gates whether the background scan is started at all — it only counts slots registered with `requires_ble == true`, so RC-emulation cameras (which never use BLE) do not keep the scanner running permanently.

---

## Key Types

### `camera_driver_t`

```c
struct camera_driver {
    esp_err_t                 (*start_recording)(void *ctx);
    esp_err_t                 (*stop_recording)(void *ctx);
    camera_recording_status_t (*get_recording_status)(void *ctx); // non-blocking cache read
    void                      (*teardown)(void *ctx);              // nullable
    void                      (*update_slot_index)(void *ctx, int new_slot); // nullable
};
```

`get_recording_status` must be non-blocking — it is called from the mismatch timer callback while the slot mutex is held.

### `camera_slot_info_t`

```c
typedef struct {
    int               index;
    char              name[32];
    camera_model_t    model;
    uint8_t           mac[6];
    bool              is_configured;
    cam_ble_status_t  ble_status;
    wifi_cam_status_t wifi_status;
    bool              is_recording;
    desired_recording_t desired_recording;
    uint32_t          ip_addr;
} camera_slot_info_t;
```

### `camera_can_state_t`

| Value | Meaning |
|-------|---------|
| `CAMERA_CAN_STATE_UNDEFINED` (0) | Slot not configured |
| `CAMERA_CAN_STATE_DISCONNECTED` (1) | Configured but not on network |
| `CAMERA_CAN_STATE_IDLE` (2) | Connected, not recording |
| `CAMERA_CAN_STATE_RECORDING` (3) | Connected and actively recording |

Values map directly to RaceCapture direct-CAN channel bytes and must not be reordered.

---

## Threading

A single FreeRTOS mutex (`s_mutex`) guards the slot array. The mismatch timer callback acquires the mutex briefly to read state and write the grace-period flag; it releases the mutex before issuing `start_recording()` / `stop_recording()` to avoid holding the lock during a potentially slow driver call.

`stop_poll_timer()` is always called **before** acquiring the mutex (in `on_wifi_disconnected` and `remove_slot`) because the timer callback itself acquires the mutex — holding it while stopping the timer would deadlock if a callback fired concurrently.

---

## Error Handling

| Scenario | Behaviour |
|----------|-----------|
| NVS schema version mismatch | Slot discarded; re-pairing required; logged as warning |
| NVS read error (non-not-found) | Slot left unconfigured; logged as warning |
| NVS write error | Logged as error; in-RAM state unaffected |
| `save_slot` with `model == UNKNOWN` | Returns `ESP_ERR_INVALID_ARG`; logged as error |
| `register_driver` table full | Returns `ESP_ERR_NO_MEM`; logged as error |
| No driver found for a loaded slot's model | Logged as warning; slot operational but recording commands are no-ops |
| Poll timer create failure | Logged as error; slot stays at `WIFI_CAM_READY` but no mismatch correction runs |
| `remove_slot` on out-of-range index | Returns `ESP_ERR_INVALID_ARG` |

#pragma once

#include "camera_types.h"
#include "esp_err.h"
#include "host/ble_hs.h"   /* ble_addr_t, BLE_HS_CONN_HANDLE_NONE */

#define CAMERA_MAX_SLOTS 4

/* ---- Driver vtable (§8 / §13.5) ---- */
typedef struct camera_driver camera_driver_t;
struct camera_driver {
    esp_err_t                  (*start_recording)(void *ctx);
    esp_err_t                  (*stop_recording)(void *ctx);
    /* Non-blocking cache read — safe from any context (§8) */
    camera_recording_status_t  (*get_recording_status)(void *ctx);
    /* nullable — called by camera_manager_remove_slot() (§20.5) */
    void                       (*teardown)(void *ctx);
    /* nullable — notifies driver of new slot index after compaction (§20.5) */
    void                       (*update_slot_index)(void *ctx, int new_slot);
};

/* ---- Public slot info struct (§9) ---- */
typedef struct {
    int               index;
    char              name[32];
    camera_model_t    model;
    uint8_t           mac[6];
    bool              is_configured;
    cam_ble_status_t  ble_status;
    wifi_cam_status_t wifi_status;
    bool              is_recording;        /* wifi_status==READY && driver reports ACTIVE */
    desired_recording_t desired_recording;
    uint32_t          ip_addr;             /* 0 when not connected */
} camera_slot_info_t;

/* ---- CAN camera state (§14.2) ---- */
typedef enum {
    CAMERA_CAN_STATE_UNDEFINED    = 0,
    CAMERA_CAN_STATE_DISCONNECTED = 1,
    CAMERA_CAN_STATE_IDLE         = 2,
    CAMERA_CAN_STATE_RECORDING    = 3,
} camera_can_state_t;

/* ---- Driver registration types (§21.4) ---- */
typedef bool   (*camera_model_match_fn)(camera_model_t model);
typedef void  *(*camera_ctx_create_fn)(int slot);

/* ==========================================================================
 * Public API
 * ========================================================================== */

/*
 * Load all cam_N/camera NVS records into RAM.  All statuses start at NONE.
 * Must be called before any other camera_manager function.
 */
void camera_manager_init(void);

/*
 * Register a driver.  camera_manager immediately assigns it to all already-
 * loaded slots whose model satisfies matches().  Called by driver _init()
 * functions before wifi/BLE stacks are started (§21.4).
 *
 * requires_ble: pass true for COHN drivers (BLE connection is maintained as a
 *   re-provisioning fallback); false for RC-emulation drivers.  This flag
 *   controls whether a slot counts as "disconnected" for the ble_core
 *   has_disconnected_cameras() gate (§12.9).
 */
esp_err_t camera_manager_register_driver(const camera_driver_t *driver,
                                          camera_model_match_fn   matches,
                                          camera_ctx_create_fn    create_ctx,
                                          bool                    requires_ble);

/* ----- Slot lookup ----- */

/* Returns slot index [0, CAMERA_MAX_SLOTS), or -1 if not found. */
int camera_manager_find_by_mac(const uint8_t mac[6]);

/*
 * Allocate a new slot for an unknown camera discovered over BLE.
 * Sets placeholder name, model = UNKNOWN.  Does NOT write NVS (model must be
 * set before saving).  Returns slot index or -1 if full.
 */
int camera_manager_register_new(const uint8_t mac[6]);

/* ----- BLE state transitions (called by open_gopro_ble) ----- */

void camera_manager_on_ble_connected(int slot, uint16_t conn_handle);
void camera_manager_on_ble_ready(int slot);      /* CAM_BLE_READY */
void camera_manager_on_ble_disconnected_by_handle(uint16_t conn_handle);

/* ----- Slot field updates ----- */

/* Set model and immediately try to assign a matching registered driver. */
void camera_manager_set_model(int slot, camera_model_t model);
void camera_manager_set_name(int slot, const char *name);

/*
 * Called by open_gopro_ble after COHN provisioning completes.
 * ready=true  → WIFI_CAM_CONNECTED (IP assigned; driver probe can now proceed)
 * ready=false → WIFI_CAM_NONE
 */
void camera_manager_set_camera_ready(int slot, bool ready);

/*
 * Called by the driver after its probe succeeds.
 * Sets WIFI_CAM_READY and starts the per-slot mismatch poll timer.
 */
void camera_manager_on_wifi_connected(int slot, uint32_t ip);

/* Called when the camera leaves the SoftAP.  Stops the poll timer. */
void camera_manager_on_wifi_disconnected(int slot);

/*
 * Called from the wifi_manager on_station_ip callback.
 * Updates last_ip for the matching slot (if any).
 */
void camera_manager_on_station_ip(const uint8_t mac[6], uint32_t ip);

/* ----- NVS persistence ----- */

/* Returns ESP_ERR_INVALID_ARG if model == CAMERA_MODEL_UNKNOWN (§6.1). */
esp_err_t camera_manager_save_slot(int slot);

/* ----- Queries ----- */

uint32_t           camera_manager_get_last_ip(int slot);
camera_model_t     camera_manager_get_model(int slot);
int                camera_manager_get_slot_count(void);

/* Copies slot state into *out.  Returns ESP_ERR_INVALID_ARG for bad index. */
esp_err_t          camera_manager_get_slot_info(int slot, camera_slot_info_t *out);

/* Translation for CAN 0x601 frame payload (§14.2). */
camera_can_state_t camera_manager_get_slot_can_state(int slot);

/* ----- Recording intent (§13) ----- */

/* Called by CAN manager on every received 0x600 frame (idempotent). */
void camera_manager_set_desired_recording_all(desired_recording_t intent);

/* Called by web UI for manual per-slot control. */
void camera_manager_set_desired_recording_slot(int slot, desired_recording_t intent);

bool camera_manager_get_auto_control(void);
void camera_manager_set_auto_control(bool enabled);

/* ----- Slot removal with compaction (§20.5) ----- */

esp_err_t camera_manager_remove_slot(int slot);

/* ----- Callbacks for ble_core (§12.9) -----
 *
 * Pass these as function pointers when constructing ble_core_callbacks_t
 * in open_gopro_ble_init().  Signatures must match ble_core_is_known_addr_cb_t
 * and ble_core_has_disconnected_cameras_cb_t respectively.
 */
bool camera_manager_is_known_ble_addr(ble_addr_t addr);
bool camera_manager_has_disconnected_cameras(void);

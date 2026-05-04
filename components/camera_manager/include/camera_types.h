/*
 * camera_types.h — Pure C types shared between camera_manager and any
 * host-side unit-test targets (§23.2).  No ESP-IDF headers included here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ---- Model identification (§5.1) ---- */
typedef enum {
    CAMERA_MODEL_UNKNOWN            = 0,

    /* GoPro RC-emulation (project-assigned IDs — user selects at pairing) */
    CAMERA_MODEL_GOPRO_HERO4_BLACK  = 40,
    CAMERA_MODEL_GOPRO_HERO4_SILVER = 41,

    /* GoPro BLE */
    CAMERA_MODEL_GOPRO_HERO7_BLACK  = 30,
    CAMERA_MODEL_GOPRO_HERO9_BLACK  = 55,
    CAMERA_MODEL_GOPRO_HERO10_BLACK = 57,
    CAMERA_MODEL_GOPRO_HERO11_BLACK = 58,
    CAMERA_MODEL_GOPRO_HERO11_MINI  = 60,
    CAMERA_MODEL_GOPRO_HERO12_BLACK = 62,
    CAMERA_MODEL_GOPRO_MAX2         = 64,
    CAMERA_MODEL_GOPRO_HERO13_BLACK = 65,
    CAMERA_MODEL_GOPRO_LIT_HERO     = 70,

    /* Future manufacturer blocks: 1000+ */
} camera_model_t;

/* ---- BLE status (§7.2) ---- */
typedef enum {
    CAM_BLE_NONE = 0,   /* RC-emulation camera, or COHN camera not yet contacted   */
    CAM_BLE_CONNECTING, /* Any in-progress BLE work: scan, connect, bond, provision */
    CAM_BLE_CONNECTED,  /* L2 up, manufacturer-specific setup in progress           */
    CAM_BLE_READY,      /* Setup complete; held open as WiFi re-provision fallback  */
} cam_ble_status_t;

/* ---- WiFi / network status (§7.3) ---- */
typedef enum {
    WIFI_CAM_NONE = 0,  /* Not on network                                          */
    WIFI_CAM_ASSOCIATING,
    WIFI_CAM_ASSOCIATED,
    WIFI_CAM_CONNECTED, /* IP assigned; driver probe pending                        */
    WIFI_CAM_PROBING,
    WIFI_CAM_READY,     /* Camera confirmed ready for recording commands            */
} wifi_cam_status_t;

/* ---- Recording status (§7.4) ---- */
typedef enum {
    CAMERA_RECORDING_UNKNOWN = 0,
    CAMERA_RECORDING_IDLE,
    CAMERA_RECORDING_ACTIVE,
} camera_recording_status_t;

/* ---- Desired recording intent (§7.5) ---- */
typedef enum {
    DESIRED_RECORDING_UNKNOWN = 0,  /* No intent yet — mismatch correction suppressed */
    DESIRED_RECORDING_STOP    = 1,
    DESIRED_RECORDING_START   = 2,
} desired_recording_t;

/* ---- Mismatch correction action (§23.1) ---- */
typedef enum {
    MISMATCH_ACTION_NONE  = 0,
    MISMATCH_ACTION_START = 1,
    MISMATCH_ACTION_STOP  = 2,
} mismatch_action_t;

/* ---- Pair-attempt state machine (BLE add-camera flow) ---- */
typedef enum {
    PAIR_ATTEMPT_IDLE = 0,        /* No attempt has been started */
    PAIR_ATTEMPT_CONNECTING,      /* BLE L2 connect in flight */
    PAIR_ATTEMPT_BONDING,         /* L2 up, waiting for encrypted bond */
    PAIR_ATTEMPT_PROVISIONING,    /* Slot registered, readiness sequence running */
    PAIR_ATTEMPT_SUCCESS,         /* Slot persisted; camera ready */
    PAIR_ATTEMPT_FAILED,          /* error_code + error_message valid */
} pair_attempt_state_t;

typedef enum {
    PAIR_ERROR_NONE = 0,
    PAIR_ERROR_SLOTS_FULL,         /* CAMERA_MAX_SLOTS reached */
    PAIR_ERROR_BLE_CONNECT_FAILED, /* ble_gap_connect failed / timed out */
    PAIR_ERROR_BOND_FAILED,        /* encryption / bond rejected */
    PAIR_ERROR_HWINFO_TIMEOUT,     /* GetHardwareInfo retry budget exhausted */
    PAIR_ERROR_MODEL_UNSUPPORTED,  /* Frozen / unsupported model */
    PAIR_ERROR_HANDSHAKE_TIMEOUT,  /* SetThirdPartyClient/SetCameraControlStatus failed */
    PAIR_ERROR_DISCONNECTED,       /* BLE dropped before reaching READY */
    PAIR_ERROR_CANCELLED,          /* POST /api/pair/cancel */
    PAIR_ERROR_INTERNAL,           /* Catch-all */
} pair_attempt_error_t;

/*
 * Pure mismatch step function — no side effects, no platform dependencies.
 * Exported here so mismatch.c can be compiled for host-side unit tests
 * without pulling in ESP-IDF headers (§23.2).
 */
mismatch_action_t mismatch_step(desired_recording_t       desired,
                                 camera_recording_status_t actual,
                                 bool                      grace_period_active);

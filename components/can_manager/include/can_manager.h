/*
 * can_manager.h — CAN bus manager public API (§14).
 *
 * Receives 0x600 logging commands and 0x602 UTC timestamps from RaceCapture.
 * Transmits 0x601 camera status at 5 Hz.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ---- Logging state (§14.2) ----------------------------------------------- */

typedef enum {
    LOGGING_STATE_UNKNOWN = 0,   /* No 0x600 frame received, or 5 s timeout elapsed */
    LOGGING_STATE_NOT_LOGGING,
    LOGGING_STATE_LOGGING,
} can_logging_state_t;

/* ---- Callback types (§14.5) ---------------------------------------------- */

/* Every received frame — for development and bus sniffing. */
typedef void (*can_rx_frame_cb_t)(uint32_t id, const uint8_t *data,
                                   uint8_t len, void *arg);

/* Every received 0x600 frame; never called with LOGGING_STATE_UNKNOWN. */
typedef void (*can_logging_state_cb_t)(can_logging_state_t state, void *arg);

/* Exactly once, on the first valid 0x602 frame (year > 2020). */
typedef void (*can_utc_acquired_cb_t)(uint64_t utc_ms, void *arg);

/* ---- Callback registration struct ---------------------------------------- */

typedef struct {
    can_rx_frame_cb_t       on_rx_frame;           /* nullable */
    void                   *on_rx_frame_arg;
    can_logging_state_cb_t  on_logging_state;       /* nullable */
    void                   *on_logging_state_arg;
    can_utc_acquired_cb_t   on_utc_acquired;        /* nullable */
    void                   *on_utc_acquired_arg;
} can_manager_callbacks_t;

/* ==========================================================================
 * Public API
 * ========================================================================== */

/*
 * Register callbacks.  Must be called before can_manager_init().
 * Struct is copied by value; caller does not need to keep it alive.
 * All fields are NULL-safe.
 */
void can_manager_register_callbacks(const can_manager_callbacks_t *cbs);

/*
 * Start the TWAI driver, RX task, TX timer, and 5 s watchdog.
 * Load timezone offset from NVS.
 * Must be called after camera_manager_init().
 */
void can_manager_init(void);

/* Current logging state — safe to call from any task. */
can_logging_state_t can_manager_get_logging_state(void);

/*
 * Returns the estimated current UTC as a Unix epoch in milliseconds,
 * extrapolated from the last received 0x602 frame using esp_timer_get_time().
 * Returns false (and leaves *out_ms unchanged) until the first valid frame.
 */
bool can_manager_get_utc_ms(uint64_t *out_ms);

/*
 * Persist a UTC-to-local offset in NVS (§14.3).
 * Clamped to IANA valid range [−12, +14].
 */
void   can_manager_set_tz_offset(int8_t hours);
int8_t can_manager_get_tz_offset(void);

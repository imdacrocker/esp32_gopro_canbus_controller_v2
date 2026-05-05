/*
 * gopro_model.h — GoPro-specific camera_model_t capability helpers (§5.2).
 *
 * These helpers are called ONLY by gopro components.  camera_manager never
 * imports this header — all behavioral branching in higher-level components
 * goes through these inline predicates rather than comparing raw model values.
 *
 * Each helper enumerates every known model explicitly.  A new model ID must
 * be consciously added to each applicable helper — there is no silent auto-
 * inclusion based on a numeric range.
 *
 * Intentionally free of ESP-IDF headers so this file can be included by
 * host-side unit tests (§23.2).
 *
 * Hero5/Hero7 BLE control is reportedly functional but not officially
 * supported by Open GoPro and not enumerated here until verified on hardware.
 */
#pragma once

#include "camera_types.h"

/** True if the model is any known GoPro camera (RC-emulation or BLE-control). */
static inline bool gopro_model_is_gopro(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO_LEGACY_RC
        || model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER
        || model == CAMERA_MODEL_GOPRO_HERO7_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO9_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO10_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_MINI
        || model == CAMERA_MODEL_GOPRO_HERO12_BLACK
        || model == CAMERA_MODEL_GOPRO_MAX2
        || model == CAMERA_MODEL_GOPRO_HERO13_BLACK
        || model == CAMERA_MODEL_GOPRO_LIT_HERO;
}

/** Camera connects by emulating a GoPro WiFi Remote AP. */
static inline bool gopro_model_uses_rc_emulation(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO_LEGACY_RC
        || model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER;
}

/**
 * Camera responds to HTTP commands on its STA-interface IP.
 *
 * Hero4 onwards run an HTTP server on whatever DHCP-assigned IP they get from
 * an external SoftAP, so we can issue gpControl GETs (status, date_time,
 * shutter — though we use UDP for shutter for latency).
 *
 * Hero3-class cameras (CAMERA_MODEL_GOPRO_HERO_LEGACY_RC) do not — port 80
 * RSTs immediately on the STA interface — so HTTP-only commands (notably
 * date/time set) silently skip on those slots.
 *
 * The model is established at pair time by the HTTP `/gp/gpControl` identify
 * probe (see camera_manager_design.md §17.5); this predicate is the gating
 * check used by `rc_send_datetime`.
 *
 * Add Hero5/6/7/etc once their STA-mode HTTP behaviour is verified on
 * hardware — model_number values from the JSON info block are logged at
 * pair time for that purpose.
 */
static inline bool gopro_model_supports_http_datetime(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER;
}

/**
 * Map a `model_name` string from the HTTP `/gp/gpControl` info JSON to a
 * `camera_model_t` enum value.  Returns CAMERA_MODEL_GOPRO_HERO_LEGACY_RC for
 * an unrecognised name (camera responded to gpControl but its model isn't yet
 * in the enum — let it run on the legacy path until the mapping is added).
 * Returns CAMERA_MODEL_UNKNOWN only when `name == NULL`.
 *
 * Implementation lives in `gopro_model.c` — string-compare table is too long
 * to keep inline.
 */
camera_model_t gopro_model_from_name(const char *model_name);

/** Camera is controlled over BLE (no WiFi association required). */
static inline bool gopro_model_uses_ble_control(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO7_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO9_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO10_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO11_MINI
        || model == CAMERA_MODEL_GOPRO_HERO12_BLACK
        || model == CAMERA_MODEL_GOPRO_MAX2
        || model == CAMERA_MODEL_GOPRO_HERO13_BLACK
        || model == CAMERA_MODEL_GOPRO_LIT_HERO;
}

/** Keepalive must be sent over UDP; all other commands use HTTP. */
static inline bool gopro_model_uses_udp_keepalive(camera_model_t model)
{
    return gopro_model_uses_rc_emulation(model);
}

/**
 * Camera predates the OpenGoPro spec and uses the legacy BLE control protocol.
 *
 * Implications today:
 *   - Initial mode is selected with SetMode (TLV cmd 0x02) rather than
 *     Load Preset Group (cmd 0x3E) used on newer cameras.
 *
 * Add Hero5/6/8 here once their model_num values are confirmed against
 * GetHardwareInfo on real hardware.
 */
static inline bool gopro_model_uses_legacy_ble(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO7_BLACK;
}

/**
 * Camera is recognised but support is intentionally frozen — when one is
 * detected after pairing, the BLE connection is dropped and the slot is
 * removed.  Other Hero7-related code (legacy predicate, SetMode sender,
 * SetThirdPartyClient wait) is kept for future investigation.
 *
 * Hero7 (firmware HD7.01.01.90.00 and similar): the camera never clears its
 * "Pairing…" screen and rejects SetShutter with status 0x02, even when driven
 * by reference implementations like gopro-ble-py.  Suspected to require an
 * undocumented authenticated handshake added in later Hero7 firmware.
 */
static inline bool gopro_model_is_frozen(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO7_BLACK;
}

/**
 * Model cannot be read from the camera — must be selected by the user at
 * pairing time (web UI pairing flow for RC-emulation cameras).
 */
static inline bool gopro_model_requires_manual_id(camera_model_t model)
{
    return gopro_model_uses_rc_emulation(model);
}

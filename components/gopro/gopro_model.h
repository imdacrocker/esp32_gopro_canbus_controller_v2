/*
 * gopro_model.h — GoPro-specific camera_model_t capability helpers (§5.2).
 *
 * These helpers are called ONLY by gopro/* components.  camera_manager never
 * imports this header — all behavioral branching in higher-level components
 * goes through these inline predicates rather than comparing raw model values.
 *
 * Each helper enumerates every known model explicitly.  A new model ID must
 * be consciously added to each applicable helper — there is no silent auto-
 * inclusion based on a numeric range.
 *
 * Intentionally free of ESP-IDF headers so this file can be included by
 * host-side unit tests (§23.2).
 */
#pragma once

#include "camera_types.h"

/** True if the model is any known GoPro camera (RC-emulation or COHN). */
static inline bool gopro_model_is_gopro(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER
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
    return model == CAMERA_MODEL_GOPRO_HERO4_BLACK
        || model == CAMERA_MODEL_GOPRO_HERO4_SILVER;
}

/** Camera is controlled via COHN HTTPS after joining the SoftAP. */
static inline bool gopro_model_uses_cohn(camera_model_t model)
{
    return model == CAMERA_MODEL_GOPRO_HERO9_BLACK
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
 * Model cannot be read from the camera — must be selected by the user at
 * pairing time (web UI pairing flow for RC-emulation cameras).
 */
static inline bool gopro_model_requires_manual_id(camera_model_t model)
{
    return gopro_model_uses_rc_emulation(model);
}

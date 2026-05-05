/*
 * gopro_model.c — Implementation of the non-inline helpers from gopro_model.h.
 *
 * Currently just `gopro_model_from_name()`: a string-compare lookup mapping
 * GoPro `info.model_name` JSON values to camera_model_t enum values.  Used by
 * the gopro_wifi_rc HTTP identify probe (§17.2.5 of camera_manager_design.md).
 *
 * Naming convention: the strings used in the table are the exact values
 * emitted by the camera's GET /gp/gpControl JSON `info.model_name` field —
 * verified against the gpControl-*.json reference dumps in goprowifihack.
 *
 * This file is compiled into the gopro_wifi_rc component (see its
 * CMakeLists.txt SRCS list).
 */

#include <string.h>
#include "gopro_model.h"

/*
 * model_name → camera_model_t.  Strings are case-sensitive.
 *
 * Hero5/6/7/8 models that respond to /gp/gpControl will land in the legacy
 * fallback until their model_name strings are confirmed against real
 * hardware and added below — the wifi_rc identify handler logs every
 * unrecognised model_name + model_number it sees, so the table can be
 * extended without further hardware testing for those known values.
 */
static const struct {
    const char    *name;
    camera_model_t model;
} k_model_table[] = {
    /* Verified against goprowifihack/HERO4/gpControl-Hero4*.json:
     *   HERO4 Black  → model_number 13
     *   HERO4 Silver → model_number 12 */
    { "HERO4 Black",  CAMERA_MODEL_GOPRO_HERO4_BLACK  },
    { "HERO4 Silver", CAMERA_MODEL_GOPRO_HERO4_SILVER },

    /* TODO(§17.13): add Hero5+ entries as their model_name strings are
     * observed at pair time on real hardware.  Reference values from
     * goprowifihack/CameraCodenames.md and gpControl-*.json:
     *   "HERO4 Session" → 16
     *   "HERO5 Black"   → 19
     *   "HERO5 Session" → 21
     *   "HERO6 Black"   → 24
     *   "HERO7 Black"   → ?  (HERO7 is currently a BLE-control model in
     *                          camera_types.h; if Hero7 in RC mode also
     *                          identifies via gpControl, a separate
     *                          enumeration may be needed)
     *   "HERO+",        "HERO+ LCD", "HERO 2018"
     */
};

camera_model_t gopro_model_from_name(const char *model_name)
{
    if (!model_name) return CAMERA_MODEL_UNKNOWN;

    for (size_t i = 0; i < sizeof(k_model_table) / sizeof(k_model_table[0]); i++) {
        if (strcmp(model_name, k_model_table[i].name) == 0) {
            return k_model_table[i].model;
        }
    }

    /* Camera responded to gpControl (so HTTP works) but the name isn't in
     * our table yet.  Falling back to the legacy enum is conservative — the
     * caller (rc_handle_http_identify) will have logged the model_name and
     * model_number at INFO so the table can be extended later. */
    return CAMERA_MODEL_GOPRO_HERO_LEGACY_RC;
}

/*
 * status.c — Periodic UDP `st` poll and binary status-response parser.
 *
 * The poll handler runs every RC_STATUS_POLL_INTERVAL_MS on the work task and
 * fires one UDP `st` datagram at every slot with last_ip != 0 (not just
 * wifi_ready ones — newly-added slots can't promote until they receive their
 * first response, see §17.5.1).
 *
 * The parser runs on the UDP RX task when an opcode-`st` datagram arrives and
 * decodes bytes 13/14/15 into ctx->recording_status.  recording_status is a
 * single enum (≤ 4 B) written only here; aligned 32-bit stores are atomic on
 * Xtensa LX7, so the single-writer / single-reader pattern (work-task readers
 * via `get_recording_status`) needs no mutex.
 *
 * §17.2.4, §17.4.1, §17.8 of camera_manager_design.md.
 */

#include "esp_log.h"
#include "camera_manager.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/status";

/* ---- Status-response decode (§17.2.4) ------------------------------------ */

void rc_parse_st_response(int slot, const uint8_t *buf, int len)
{
    if (slot < 0 || slot >= CAMERA_MAX_SLOTS) return;
    if (len < RC_RESP_MIN_BYTES) return;

    gopro_wifi_rc_ctx_t *ctx = &s_ctx[slot];

    uint8_t pwr   = buf[RC_RESP_PWR_OFFSET];
    uint8_t state = buf[RC_RESP_STATE_OFFSET];

    camera_recording_status_t next;
    if (pwr == 1) {
        /* Camera reports off/sleeping — recording state is meaningless. */
        next = CAMERA_RECORDING_UNKNOWN;
    } else if (state == 1) {
        next = CAMERA_RECORDING_ACTIVE;
    } else {
        next = CAMERA_RECORDING_IDLE;
    }

    if (next != ctx->recording_status) {
        ESP_LOGD(TAG, "slot %d: %s → %s",
                 slot,
                 ctx->recording_status == CAMERA_RECORDING_ACTIVE ? "recording" :
                 ctx->recording_status == CAMERA_RECORDING_IDLE   ? "idle"      :
                                                                    "unknown",
                 next == CAMERA_RECORDING_ACTIVE ? "recording" :
                 next == CAMERA_RECORDING_IDLE   ? "idle"      :
                                                   "unknown");
    }
    ctx->recording_status = next;
}

/* ---- Status poll (§17.8) ------------------------------------------------- */

/*
 * Called every RC_STATUS_POLL_INTERVAL_MS from the work task.  Sends one UDP
 * `st` request to each slot that has an IP — not gated on wifi_ready so that
 * newly-added slots can promote on the first response.  Replies are handled
 * asynchronously by rc_udp_rx_task → rc_parse_st_response().
 */
void rc_handle_status_poll_all(void)
{
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        gopro_wifi_rc_ctx_t *ctx = &s_ctx[i];
        if (ctx->last_ip == 0) continue;
        rc_send_st(ctx->last_ip);
    }
}

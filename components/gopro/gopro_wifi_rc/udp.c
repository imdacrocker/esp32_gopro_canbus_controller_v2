/*
 * udp.c — UDP socket init, keepalive TX, Wake-on-LAN TX, and RX task.
 *
 * Transports:
 *   TX keepalive : UDP unicast  → camera IP : port 8484
 *   TX WoL       : UDP broadcast → 255.255.255.255 : port 9
 *   RX ACKs      : UDP bound on port 8383; first byte 0x5F = keepalive ACK
 *
 * §17.2, §17.6, §17.8 of camera_manager_design.md.
 */

#include <string.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_manager.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/udp";

/* ---- UDP socket init ----------------------------------------------------- */

/*
 * Open a single TX socket used for both keepalive unicasts and WoL broadcasts.
 * The socket is unbound (OS assigns an ephemeral source port).
 * SO_BROADCAST must be enabled for WoL packets.
 */
esp_err_t rc_udp_init(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return ESP_FAIL;
    }

    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                   &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGE(TAG, "setsockopt SO_BROADCAST failed: %d", errno);
        close(sock);
        return ESP_FAIL;
    }

    s_udp_sock = sock;
    ESP_LOGI(TAG, "TX socket opened (fd=%d)", sock);
    return ESP_OK;
}

/* ---- Keepalive TX -------------------------------------------------------- */

void rc_send_keepalive(uint32_t ip)
{
    if (s_udp_sock < 0 || ip == 0) return;

    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RC_UDP_TX_PORT),
        .sin_addr.s_addr = ip,
    };
    const char *payload = RC_UDP_KEEPALIVE_PAYLOAD;
    sendto(s_udp_sock, payload, strlen(payload), 0,
           (struct sockaddr *)&dst, sizeof(dst));
}

/* ---- Wake-on-LAN TX ------------------------------------------------------ */

/*
 * Send RC_WOL_BURST standard magic packets to 255.255.255.255:9 targeting mac.
 * Magic packet = 6 × 0xFF followed by the target MAC repeated 16 times (102 B).
 */
void rc_send_wol(uint32_t ip, const uint8_t mac[6])
{
    if (s_udp_sock < 0) return;

    uint8_t pkt[102];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(pkt + 6 + i * 6, mac, 6);
    }

    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RC_UDP_WOL_PORT),
        .sin_addr.s_addr = IPADDR_BROADCAST,
    };

    for (int i = 0; i < RC_WOL_BURST; i++) {
        sendto(s_udp_sock, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&dst, sizeof(dst));
    }

    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));
    ESP_LOGD(TAG, "WoL burst ×%d → %s (mac %02x:%02x:%02x:%02x:%02x:%02x)",
             RC_WOL_BURST, ip_str,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- UDP RX task --------------------------------------------------------- */

/*
 * Bind a dedicated socket to RC_UDP_RX_PORT and loop forever.
 * On each datagram:
 *   - If first byte == 0x5F (keepalive ACK), find the slot by source IP and
 *     update ctx->last_keepalive_ack.  If the slot was not yet wifi_ready,
 *     post CMD_PROBE so the work task probes it.
 *   - All other payloads are logged at VERBOSE and discarded.
 *
 * last_keepalive_ack is a TickType_t (32-bit on ESP32).  Xtensa LX7 guarantees
 * aligned 32-bit stores are atomic, so no mutex is needed for the single writer
 * (this task) and single reader (work task in rc_handle_keepalive_tick).
 */
void rc_udp_rx_task(void *arg)
{
    (void)arg;

    int rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx_sock < 0) {
        ESP_LOGE(TAG, "RX socket() failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* Allow reuse so a firmware restart doesn't leave the port locked. */
    int reuse = 1;
    setsockopt(rx_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RC_UDP_RX_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(rx_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind() port %d failed: %d", RC_UDP_RX_PORT, errno);
        close(rx_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RX socket bound to port %d (fd=%d)", RC_UDP_RX_PORT, rx_sock);

    uint8_t buf[64];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    while (1) {
        int n = recvfrom(rx_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &src_len);
        if (n <= 0) continue;

        if (n < 1 || buf[0] != RC_UDP_KEEPALIVE_ACK_BYTE) {
            ESP_LOGV(TAG, "RX: ignoring %d-byte UDP (first byte 0x%02x)", n, buf[0]);
            continue;
        }

        /* Keepalive ACK — find the slot whose last_ip matches the sender. */
        uint32_t src_ip = src.sin_addr.s_addr;
        int matched = -1;
        for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
            if (s_ctx[i].last_ip == src_ip && s_ctx[i].last_ip != 0) {
                matched = i;
                break;
            }
        }
        if (matched < 0) {
            ESP_LOGV(TAG, "RX: keepalive ACK from unknown IP");
            continue;
        }

        gopro_wifi_rc_ctx_t *ctx = &s_ctx[matched];
        ctx->last_keepalive_ack = xTaskGetTickCount();

        if (!ctx->wifi_ready) {
            rc_work_cmd_t cmd = { .type = RC_CMD_PROBE,
                                  .slot_cmd = { .slot = matched } };
            xQueueSend(s_work_queue, &cmd, 0);
        }
    }
}

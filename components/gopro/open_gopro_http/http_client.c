/*
 * http_client.c — HTTPS GET helper for GoPro COHN cameras.
 *
 * Builds a URL from the slot's IP address, attaches HTTP Basic Auth, and
 * issues a single synchronous GET.  The camera uses a self-signed TLS cert;
 * peer verification is skipped (private LAN — requires CONFIG_ESP_TLS_INSECURE
 * and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY in sdkconfig).
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "lwip/ip4_addr.h"
#include "open_gopro_http_internal.h"

static const char *TAG = "gopro_http/client";

/* ---- esp_http_client event handler --------------------------------------- */

typedef struct {
    char  *buf;
    size_t buf_len;
    size_t written;
} resp_state_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_state_t *rs = (resp_state_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && rs && rs->buf) {
        size_t remaining = rs->buf_len - 1 - rs->written;
        size_t copy      = (size_t)evt->data_len < remaining
                         ? (size_t)evt->data_len : remaining;
        if (copy > 0) {
            memcpy(rs->buf + rs->written, evt->data, copy);
            rs->written += copy;
        }
    }
    return ESP_OK;
}

/* ---- Public API ---------------------------------------------------------- */

int gopro_http_get(gopro_http_ctx_t *ctx, const char *path,
                   char *resp_buf, size_t buf_len)
{
    /* Format camera IP as dotted-decimal.
     * wifi_manager passes the IPv4 address in network byte order (big-endian). */
    char ip_str[16];
    ip4_addr_t addr = { .addr = ctx->ip };
    ip4addr_ntoa_r(&addr, ip_str, sizeof(ip_str));

    char url[GOPRO_HTTP_URL_MAX];
    snprintf(url, sizeof(url), "https://%s:%d%s", ip_str, GOPRO_HTTP_PORT, path);

    resp_state_t rs = { .buf = resp_buf, .buf_len = buf_len, .written = 0 };

    esp_http_client_config_t cfg = {
        .url                      = url,
        .method                   = HTTP_METHOD_GET,
        .username                 = ctx->user,
        .password                 = ctx->pass,
        .auth_type                = HTTP_AUTH_TYPE_BASIC,
        .timeout_ms               = GOPRO_HTTP_TIMEOUT_MS,
        .event_handler            = http_event_handler,
        .user_data                = &rs,
        /* Skip TLS peer verification — GoPro COHN uses a self-signed cert.
         * Requires CONFIG_ESP_TLS_INSECURE=y in sdkconfig. */
        .skip_cert_common_name_check = true,
        .transport_type           = HTTP_TRANSPORT_OVER_SSL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "slot %d: esp_http_client_init failed", ctx->slot);
        return -1;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = -1;
    if (err == ESP_OK) {
        status_code = esp_http_client_get_status_code(client);
        if (resp_buf && rs.written < buf_len) {
            resp_buf[rs.written] = '\0';
        }
    } else {
        ESP_LOGW(TAG, "slot %d: %s → %s",
                 ctx->slot, path, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return status_code;
}

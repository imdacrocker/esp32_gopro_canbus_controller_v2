#pragma once

#include <stddef.h>
#include "esp_err.h"

/**
 * Start the recovery HTTP server (port 80, SoftAP only).
 *
 * Registers:
 *   GET  /                        — serves the embedded HTML
 *   GET  /api/version             — running firmware identity
 *   POST /api/ota/upload-app      — stream new app image into ota_0
 *   POST /api/ota/upload-ui       — stream new storage image into LittleFS
 *   POST /api/ota/commit          — set boot partition + reboot
 *   POST /api/ota/boot-main       — boot ota_0 without uploading anything
 *
 * The HTML pointer is owned by the caller (typically points at an
 * EMBED_TXTFILES symbol), and must outlive the HTTP server.
 *
 * Returns ESP_OK or an esp_err_t from httpd_start() / handler registration.
 */
esp_err_t recovery_http_init(const char *html, size_t html_len);

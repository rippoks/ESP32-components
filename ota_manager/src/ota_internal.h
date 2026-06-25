#pragma once

#include "ota_manager.h"
#include <stdbool.h>
#include <stddef.h>

/* ── Transport ops table (internal, not public) ─────────────────── */

typedef struct {
    esp_err_t (*perform)(const char *url, ota_event_cb_t cb, void *cb_ctx);
} ota_transport_ops_t;

/* ── Transport implementations (exactly one compiled, per Kconfig) ─ */

esp_err_t ota_transport_https_perform(const char *url, ota_event_cb_t cb, void *cb_ctx);
esp_err_t ota_transport_http_perform(const char *url, ota_event_cb_t cb, void *cb_ctx);

/* ── Version comparison + fetch (ota_version.c) ─────────────────── */

bool      ota_version_is_newer(const char *remote, const char *local);
esp_err_t ota_version_fetch(const char *base_url, char *out_buf, size_t buf_len);

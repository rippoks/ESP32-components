#include <stdbool.h>
#include <stdio.h>

#ifndef UNIT_TEST
#include "esp_log.h"
#include "esp_http_client.h"
#include <string.h>
static const char *TAG = "OTA_MGR";
#endif

static bool parse_semver(const char *s, unsigned *maj, unsigned *min, unsigned *pat) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    return sscanf(s, "%u.%u.%u", maj, min, pat) == 3;
}

#ifndef UNIT_TEST
esp_err_t ota_version_fetch(const char *base_url, char *out_buf, size_t buf_len) {
    char url[256];
    int n = snprintf(url, sizeof(url), "%s.version", base_url);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);

    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t total = 0;
    while (total < buf_len - 1) {
        int rd = esp_http_client_read(client, out_buf + total, (int)(buf_len - 1 - total));
        if (rd < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (rd == 0) break;
        total += (size_t)rd;
    }
    out_buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}
#endif

bool ota_version_is_newer(const char *remote, const char *local) {
    unsigned rj, rn, rp, lj, ln, lp;
    if (!parse_semver(remote, &rj, &rn, &rp) || !parse_semver(local, &lj, &ln, &lp)) {
#ifndef UNIT_TEST
        ESP_LOGW(TAG, "ota_version_is_newer: unparseable version string");
#else
        printf("ota_version_is_newer: unparseable version string\n");
#endif
        return false;
    }
    if (rj != lj) return rj > lj;
    if (rn != ln) return rn > ln;
    return rp > lp;
}

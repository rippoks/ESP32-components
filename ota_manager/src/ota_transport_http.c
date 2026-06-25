#include "ota_internal.h"
#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "OTA_MGR";

/* ── Event-firing helpers ───────────────────────────────────────── */

static void fire_evt(ota_event_t evt, ota_event_cb_t cb, void *cb_ctx)
{
    if (cb) {
        ota_event_data_t ev = { .event = evt };
        cb(&ev, cb_ctx);
    }
}

static void fire_failed(esp_err_t err, ota_event_cb_t cb, void *cb_ctx)
{
    if (cb) {
        ota_event_data_t ev = { .event = OTA_EVENT_FAILED };
        ev.data.err = err;
        cb(&ev, cb_ctx);
    }
}

static void fire_progress(uint8_t pct, ota_event_cb_t cb, void *cb_ctx)
{
    if (cb) {
        ota_event_data_t ev = { .event = OTA_EVENT_PROGRESS };
        ev.data.progress_pct = pct;
        cb(&ev, cb_ctx);
    }
}

/* ── HTTP transport implementation ──────────────────────────────── */

esp_err_t ota_transport_http_perform(const char *url, ota_event_cb_t cb, void *cb_ctx)
{
    /* Step 1: Init HTTP client */
    esp_http_client_config_t cfg = {
        .url        = url,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        fire_failed(ESP_FAIL, cb, cb_ctx);
        return ESP_FAIL;
    }

    /* Step 2: Open connection */
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        fire_failed(err, cb, cb_ctx);
        esp_http_client_cleanup(client);
        return err;
    }

    /* Step 3: Fetch headers, check HTTP status */
    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP OTA: unexpected status %d", status);
        fire_failed(ESP_FAIL, cb, cb_ctx);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Step 4: Begin OTA on next update partition */
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        ESP_LOGE(TAG, "no OTA update partition available");
        fire_failed(ESP_ERR_NOT_FOUND, cb, cb_ctx);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        fire_failed(err, cb, cb_ctx);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    /* Step 5: Signal download start */
    fire_evt(OTA_EVENT_DOWNLOAD_START, cb, cb_ctx);

    /* Step 6: Heap-allocate read buffer */
    char *buf = malloc(4096);
    if (buf == NULL) {
        fire_failed(ESP_ERR_NO_MEM, cb, cb_ctx);
        esp_ota_abort(ota);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    /* Step 7: Download + write loop with throttled progress */
    int total    = 0;
    int last_pct = -10;  /* bucket -1 so first chunk reports 0% */
    while (1) {
        int r = esp_http_client_read(client, buf, 4096);
        if (r < 0) { err = ESP_FAIL; break; }   /* read error */
        if (r == 0) { err = ESP_OK;  break; }   /* EOF / complete */
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) { break; }            /* write error */
        total += r;
        uint8_t pct = (content_len > 0)
            ? (uint8_t)(((int64_t)total * 100) / content_len)
            : 0;
        if ((int)(pct / 10) != (int)(last_pct / 10)) {
            last_pct = pct;
            fire_progress(pct, cb, cb_ctx);
        }
    }
    free(buf);

    /* Completeness guard: loop ended cleanly but body was incomplete */
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(client)) {
        err = ESP_FAIL;
    }

    /* Step 8: Handle loop/completeness errors */
    if (err != ESP_OK) {
        fire_failed(err, cb, cb_ctx);
        esp_ota_abort(ota);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    /* Step 9: Finalise OTA */
    fire_evt(OTA_EVENT_DOWNLOAD_DONE, cb, cb_ctx);

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        fire_failed(err, cb, cb_ctx);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    fire_evt(OTA_EVENT_APPLYING, cb, cb_ctx);

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        fire_failed(err, cb, cb_ctx);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    fire_evt(OTA_EVENT_SUCCESS, cb, cb_ctx);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

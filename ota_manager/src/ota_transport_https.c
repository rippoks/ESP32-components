#include "ota_internal.h"
#include "sdkconfig.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#if !defined(CONFIG_OTA_HTTPS_INSECURE) || !CONFIG_OTA_HTTPS_INSECURE
#include "esp_crt_bundle.h"
#endif

#if CONFIG_OTA_HTTPS_INSECURE
static const char *TAG = "OTA_MGR";  /* only referenced by the insecure-mode warning */
#endif

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

/* ── HTTPS transport implementation ─────────────────────────────── */

esp_err_t ota_transport_https_perform(const char *url, ota_event_cb_t cb, void *cb_ctx)
{
    /* Step 1: Build HTTP client config */
    esp_http_client_config_t http_config = {
        .url        = url,
        .timeout_ms = 30000,
    };
#if CONFIG_OTA_HTTPS_INSECURE
    http_config.crt_bundle_attach           = NULL;
    http_config.cert_pem                    = NULL;
    http_config.skip_cert_common_name_check = true;
    ESP_LOGW(TAG, "INSECURE TLS: server certificate is NOT verified");
#else
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    /* Step 2: Wrap into HTTPS OTA config */
    esp_https_ota_config_t ota_config = { .http_config = &http_config };

    /* Step 3: Begin */
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK || handle == NULL) {
        fire_failed(err, cb, cb_ctx);
        return err;
    }

    /* Step 3b: Image-descriptor version guard */
#if CONFIG_OTA_VERSION_CHECK
    esp_app_desc_t img_desc;
    err = esp_https_ota_get_img_desc(handle, &img_desc);
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        fire_failed(err, cb, cb_ctx);
        return err;
    }
    if (!ota_version_is_newer(img_desc.version, ota_manager_current_version())) {
        esp_https_ota_abort(handle);
        fire_failed(ESP_ERR_INVALID_VERSION, cb, cb_ctx);
        return ESP_ERR_INVALID_VERSION;
    }
    /* On match, fire no event — core already fired OTA_EVENT_UPDATE_AVAILABLE */
#endif

    /* Step 4: Download loop with throttled progress */
    fire_evt(OTA_EVENT_DOWNLOAD_START, cb, cb_ctx);

    int last_pct = -1;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int image_size  = esp_https_ota_get_image_size(handle);
        int read_so_far = esp_https_ota_get_image_len_read(handle);
        uint8_t pct = (image_size > 0) ? (uint8_t)((read_so_far * 100) / image_size) : 0;
        if ((int)(pct / 10) != (int)(last_pct / 10)) {
            last_pct = pct;
            fire_progress(pct, cb, cb_ctx);
        }
    }

    /* Step 5: Handle loop result */
    if (err != ESP_OK) {
        fire_failed(err, cb, cb_ctx);
        esp_https_ota_abort(handle);
        return err;
    }

    /* Step 6: Finalise */
    fire_evt(OTA_EVENT_DOWNLOAD_DONE, cb, cb_ctx);
    fire_evt(OTA_EVENT_APPLYING, cb, cb_ctx);

    esp_err_t fin = esp_https_ota_finish(handle);
    if (fin == ESP_OK) {
        fire_evt(OTA_EVENT_SUCCESS, cb, cb_ctx);
        return ESP_OK;
    }

    fire_failed(fin, cb, cb_ctx);
    return fin;
}

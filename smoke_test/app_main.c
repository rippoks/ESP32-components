/*
 * ota_manager end-to-end smoke-test app (Task 9).
 *
 * Manual-trigger mode: connects Wi-Fi, confirms the running image healthy once
 * the network is up, then periodically calls ota_manager_check_and_update().
 *
 * Fill in WIFI_SSID / WIFI_PASS below. Build with -DPROJECT_VER=X.Y.Z so the
 * running version is what the component compares against the served .version.
 *
 * For the deliberately-bad 0.3.0 build, comment out the marked
 * ota_manager_confirm_valid() call so the healthcheck times out and rolls back.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "ota_manager.h"

#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"

#define CHECK_INTERVAL_MS (20 * 1000)   /* swap the served firmware between checks */

static const char *TAG = "SMOKE";
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

/* ── OTA event callback: log the full event sequence the smoke test checks ── */
static void ota_cb(const ota_event_data_t *ev, void *ctx)
{
    (void)ctx;
    switch (ev->event) {
        case OTA_EVENT_CHECK_START:        ESP_LOGI(TAG, "OTA: CHECK_START"); break;
        case OTA_EVENT_UPDATE_AVAILABLE:   ESP_LOGI(TAG, "OTA: UPDATE_AVAILABLE"); break;
        case OTA_EVENT_UP_TO_DATE:         ESP_LOGI(TAG, "OTA: UP_TO_DATE"); break;
        case OTA_EVENT_DOWNLOAD_START:     ESP_LOGI(TAG, "OTA: DOWNLOAD_START"); break;
        case OTA_EVENT_PROGRESS:           ESP_LOGI(TAG, "OTA: PROGRESS %d%%", ev->data.progress_pct); break;
        case OTA_EVENT_DOWNLOAD_DONE:      ESP_LOGI(TAG, "OTA: DOWNLOAD_DONE"); break;
        case OTA_EVENT_APPLYING:           ESP_LOGI(TAG, "OTA: APPLYING"); break;
        case OTA_EVENT_SUCCESS:            ESP_LOGI(TAG, "OTA: SUCCESS (reboot pending)"); break;
        case OTA_EVENT_FAILED:             ESP_LOGE(TAG, "OTA: FAILED %s", esp_err_to_name(ev->data.err)); break;
        case OTA_EVENT_ROLLBACK_TRIGGERED: ESP_LOGW(TAG, "OTA: ROLLBACK_TRIGGERED"); break;
        default: break;
    }
}

/* ── Minimal Wi-Fi station bring-up ───────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_connect_blocking(void)
{
    s_wifi_eg = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, WIFI_SSID, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, WIFI_PASS, sizeof(sta.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi \"%s\"...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi connected");
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_connect_blocking();

    ota_manager_config_t cfg = {
        .server_url        = NULL,   /* use CONFIG_OTA_SERVER_URL */
        .event_cb          = ota_cb,
        .event_cb_ctx      = NULL,
        .poll_interval_sec = 0,
    };
    ESP_ERROR_CHECK(ota_manager_init(&cfg));
    ESP_LOGI(TAG, "Running version: %s", ota_manager_current_version());

    /* ===== Healthy-image confirmation =====
     * Network is up and init succeeded → the image has proven itself.
     * For the BAD 0.3.0 build, COMMENT OUT the next line so the healthcheck
     * times out (~30 s) and the device rolls back to 0.2.0. */
    ota_manager_confirm_valid();

    /* Manual mode: drive checks ourselves. Swap the served firmware/.version
     * between iterations to walk through the smoke-test steps. On a successful
     * apply (AUTO_REBOOT=y default) check_and_update reboots and does not
     * return. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
        ESP_LOGI(TAG, "Triggering OTA check...");
        esp_err_t r = ota_manager_check_and_update();
        ESP_LOGI(TAG, "check_and_update returned: %s", esp_err_to_name(r));
    }
}

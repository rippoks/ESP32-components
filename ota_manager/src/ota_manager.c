#include "ota_manager.h"
#include "sdkconfig.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if CONFIG_OTA_ROLLBACK_ENABLED
#include "esp_timer.h"
#endif

static const char *TAG = "OTA_MGR";

/* ── Module statics ─────────────────────────────────────────────── */

static ota_manager_state_t  s_state = OTA_MGR_STATE_IDLE;
static SemaphoreHandle_t    s_mutex = NULL;
static ota_manager_config_t s_cfg;
#if CONFIG_OTA_ROLLBACK_ENABLED
static esp_timer_handle_t   s_healthcheck_timer = NULL;
#endif

/* ── Event delivery helpers ─────────────────────────────────────── */

static void ota_fire(const ota_event_data_t *ev)
{
    if (s_cfg.event_cb) {
        s_cfg.event_cb(ev, s_cfg.event_cb_ctx);
    }
}

static void ota_fire_evt(ota_event_t evt)
{
    ota_event_data_t ev = { .event = evt };
    ota_fire(&ev);
}

/* ── Healthcheck timer callback ─────────────────────────────────── */

#if CONFIG_OTA_ROLLBACK_ENABLED
#if CONFIG_OTA_HEALTHCHECK_TIMEOUT_SEC > 0
static void ota_healthcheck_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "Healthcheck timeout expired; rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}
#endif
#endif

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t ota_manager_init(const ota_manager_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Partition validation */
    if (esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL) == NULL) {
        ESP_LOGE(TAG, "Required partition ota_0 not found");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL) == NULL) {
        ESP_LOGE(TAG, "Required partition ota_1 not found");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_OTA, NULL) == NULL) {
        ESP_LOGE(TAG, "Required partition otadata not found");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Store config */
    s_cfg = *cfg;
    if (s_cfg.server_url == NULL) {
        s_cfg.server_url = CONFIG_OTA_SERVER_URL;
    }
    if (s_cfg.server_url[0] == '\0') {
        ESP_LOGW(TAG, "no server URL configured");
    }

    /* Clamp poll interval */
    if (s_cfg.poll_interval_sec == 0) {
        s_cfg.poll_interval_sec = CONFIG_OTA_POLL_INTERVAL_SEC;
    } else if (s_cfg.poll_interval_sec < 60 || s_cfg.poll_interval_sec > 86400) {
        ESP_LOGW(TAG, "poll_interval_sec %"PRIu32" clamped to [60, 86400]",
                 s_cfg.poll_interval_sec);
        if (s_cfg.poll_interval_sec < 60) {
            s_cfg.poll_interval_sec = 60;
        } else {
            s_cfg.poll_interval_sec = 86400;
        }
    }

#if CONFIG_OTA_ROLLBACK_ENABLED
    /* Rollback pending check */
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "App is PENDING_VERIFY; call ota_manager_confirm_valid() once healthy");
        }
    }

#if CONFIG_OTA_HEALTHCHECK_TIMEOUT_SEC > 0
    const esp_timer_create_args_t a = { .callback = ota_healthcheck_cb, .name = "ota_hc" };
    if (esp_timer_create(&a, &s_healthcheck_timer) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    esp_timer_start_once(s_healthcheck_timer,
                         (uint64_t)CONFIG_OTA_HEALTHCHECK_TIMEOUT_SEC * 1000000ULL);
#endif
#endif

    return ESP_OK;
}

esp_err_t ota_manager_start_task(void)
{
    ESP_LOGI(TAG, "start_task (stub)");
    return ESP_OK;
}

esp_err_t ota_manager_check_and_update(void)
{
    ESP_LOGI(TAG, "check_and_update (stub)");
    return ESP_OK;
}

void ota_manager_confirm_valid(void)
{
#if CONFIG_OTA_ROLLBACK_ENABLED
    if (s_healthcheck_timer != NULL) {
        esp_timer_stop(s_healthcheck_timer);
        esp_timer_delete(s_healthcheck_timer);
        s_healthcheck_timer = NULL;
    }
    esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback returned 0x%x", ret);
    }
#endif
}

void ota_manager_rollback(void)
{
#if CONFIG_OTA_ROLLBACK_ENABLED
    ota_fire_evt(OTA_EVENT_ROLLBACK_TRIGGERED);
    esp_ota_mark_app_invalid_rollback_and_reboot();
#endif
}

const char *ota_manager_current_version(void)
{
    return esp_app_get_description()->version;
}

ota_manager_state_t ota_manager_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ota_manager_state_t state = s_state;
    xSemaphoreGive(s_mutex);
    return state;
}

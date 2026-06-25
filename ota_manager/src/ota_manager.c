#include "ota_manager.h"
#include "ota_internal.h"
#include "sdkconfig.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#if CONFIG_OTA_ROLLBACK_ENABLED
#include "esp_timer.h"
#endif

static const char *TAG = "OTA_MGR";

/* ── Module statics ─────────────────────────────────────────────── */

static ota_manager_state_t  s_state = OTA_MGR_STATE_IDLE;
static SemaphoreHandle_t    s_mutex = NULL;
static ota_manager_config_t s_cfg;
static ota_transport_ops_t  s_transport_ops;
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

/* ── State helper ───────────────────────────────────────────────── */

static void ota_set_state(ota_manager_state_t s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = s;
    xSemaphoreGive(s_mutex);
}

/* ── Internal event trampoline ──────────────────────────────────── */

static void ota_internal_event(const ota_event_data_t *ev, void *unused)
{
    (void)unused;
    switch (ev->event) {
        case OTA_EVENT_DOWNLOAD_START: ota_set_state(OTA_MGR_STATE_DOWNLOADING); break;
        case OTA_EVENT_APPLYING:       ota_set_state(OTA_MGR_STATE_APPLYING);    break;
        default: break;
    }
    ota_fire(ev);
}

/* ── Version gate ───────────────────────────────────────────────── */

#if CONFIG_OTA_VERSION_CHECK
static esp_err_t ota_version_gate(const char *url)
{
    char remote[32];
    esp_err_t err = ota_version_fetch(url, remote, sizeof(remote));
    if (err != ESP_OK) {
        ota_event_data_t ev = { .event = OTA_EVENT_FAILED };
        ev.data.err = err;
        ota_fire(&ev);
        return err;
    }
    if (!ota_version_is_newer(remote, ota_manager_current_version())) {
        ota_fire_evt(OTA_EVENT_UP_TO_DATE);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;   /* newer; caller fires UPDATE_AVAILABLE */
}
#endif

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

    /* Clamp poll interval (only meaningful in poll mode; the CONFIG_OTA_POLL_*
     * symbols exist only when OTA_TRIGGER_POLL is selected) */
#if CONFIG_OTA_TRIGGER_POLL
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
#endif

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

    /* Transport selection */
#if CONFIG_OTA_TRANSPORT_HTTPS
    s_transport_ops.perform = ota_transport_https_perform;
#else
    s_transport_ops.perform = ota_transport_http_perform;
#endif

    return ESP_OK;
}

esp_err_t ota_manager_check_and_update(void)
{
    /* Atomic IDLE → CHECKING transition */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state != OTA_MGR_STATE_IDLE) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = OTA_MGR_STATE_CHECKING;
    xSemaphoreGive(s_mutex);

    /* Validate URL */
    const char *url = s_cfg.server_url;
    if (url == NULL || url[0] == '\0') {
        ota_set_state(OTA_MGR_STATE_IDLE);
        return ESP_ERR_INVALID_ARG;
    }

    ota_fire_evt(OTA_EVENT_CHECK_START);

#if CONFIG_OTA_VERSION_CHECK
    esp_err_t g = ota_version_gate(url);   /* fires UP_TO_DATE or FAILED itself */
    if (g != ESP_OK) {
        ota_set_state(OTA_MGR_STATE_IDLE);
        return g;
    }
#endif

    ota_fire_evt(OTA_EVENT_UPDATE_AVAILABLE);   /* exactly once, both check-on and check-off */

    esp_err_t result = s_transport_ops.perform(url, ota_internal_event, NULL);

    ota_set_state(OTA_MGR_STATE_IDLE);   /* trampoline may have moved state to DOWNLOADING/APPLYING */

    if (result == ESP_OK) {
#if CONFIG_OTA_AUTO_REBOOT
        vTaskDelay(pdMS_TO_TICKS(1000));   /* let logs/events flush */
        esp_restart();                     /* does not return */
#endif
    }
    return result;
}

#if CONFIG_OTA_TRIGGER_POLL
#include "esp_random.h"
static void ota_poll_task(void *arg)
{
    (void)arg;
    uint32_t base = s_cfg.poll_interval_sec;
    uint32_t first = esp_random() % (base < 300 ? base : 300);
    vTaskDelay(pdMS_TO_TICKS(first * 1000));
    while (1) {
        esp_err_t r = ota_manager_check_and_update();
        if (r == ESP_OK) {   /* reached only if OTA_AUTO_REBOOT=n */
            esp_restart();
        }
        uint32_t jitter = (base * CONFIG_OTA_POLL_JITTER_PCT / 100);
        uint32_t wait = base + (jitter ? (esp_random() % jitter) : 0);
        vTaskDelay(pdMS_TO_TICKS(wait * 1000));
    }
}
#endif

esp_err_t ota_manager_start_task(void)
{
#if CONFIG_OTA_TRIGGER_POLL
    BaseType_t ok = xTaskCreate(ota_poll_task, "ota_mgr",
        CONFIG_OTA_TASK_STACK_SIZE, NULL, CONFIG_OTA_TASK_PRIORITY, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
#else
    ESP_LOGI(TAG, "manual trigger mode: start_task is a no-op");
    return ESP_OK;
#endif
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

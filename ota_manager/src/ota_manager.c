#include "ota_manager.h"
#include "esp_log.h"

static const char *TAG = "OTA_MGR";

esp_err_t ota_manager_init(const ota_manager_config_t *cfg)
{
    ESP_LOGI(TAG, "init (stub)");
    (void)cfg;
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
    ESP_LOGI(TAG, "confirm_valid (stub)");
}

void ota_manager_rollback(void)
{
    ESP_LOGI(TAG, "rollback (stub)");
}

const char *ota_manager_current_version(void)
{
    return "0.0.0";
}

ota_manager_state_t ota_manager_get_state(void)
{
    return OTA_MGR_STATE_IDLE;
}

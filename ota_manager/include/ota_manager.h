#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Events ──────────────────────────────────────────────────── */

typedef enum {
    OTA_EVENT_CHECK_START,        // About to contact server
    OTA_EVENT_UPDATE_AVAILABLE,   // Version check passed; newer firmware exists
    OTA_EVENT_UP_TO_DATE,         // Version check passed; no update needed
    OTA_EVENT_DOWNLOAD_START,     // Download beginning
    OTA_EVENT_PROGRESS,           // Download in progress; data.progress_pct valid
    OTA_EVENT_DOWNLOAD_DONE,      // Download complete, verifying
    OTA_EVENT_APPLYING,           // Setting boot partition
    OTA_EVENT_SUCCESS,            // OTA complete; reboot pending or already issued
    OTA_EVENT_FAILED,             // OTA failed; data.err valid
    OTA_EVENT_ROLLBACK_TRIGGERED, // App called ota_manager_rollback(); reboot imminent
} ota_event_t;

typedef struct {
    ota_event_t event;
    union {
        uint8_t  progress_pct;  // 0–100, valid on OTA_EVENT_PROGRESS
        esp_err_t err;          // valid on OTA_EVENT_FAILED
    } data;
} ota_event_data_t;

/* ── State ───────────────────────────────────────────────────── */

typedef enum {
    OTA_MGR_STATE_IDLE,
    OTA_MGR_STATE_CHECKING,
    OTA_MGR_STATE_DOWNLOADING,
    OTA_MGR_STATE_APPLYING,
} ota_manager_state_t;

/* ── Callback ─────────────────────────────────────────────────── */

/**
 * Invoked from the OTA task (poll mode) or from the caller's task
 * (manual mode). Runs synchronously inside the OTA flow: keep it fast
 * and non-blocking — a slow callback stalls the download.
 */
typedef void (*ota_event_cb_t)(const ota_event_data_t *event, void *user_ctx);

/* ── Config ───────────────────────────────────────────────────── */

typedef struct {
    const char      *server_url;        // Override CONFIG_OTA_SERVER_URL. NULL = use Kconfig value.
    ota_event_cb_t   event_cb;          // NULL = no callback
    void            *event_cb_ctx;      // Passed to event_cb unchanged
    uint32_t         poll_interval_sec; // 0 = use CONFIG_OTA_POLL_INTERVAL_SEC. Clamped to 60–86400.
} ota_manager_config_t;

/* ── API ──────────────────────────────────────────────────────── */

/**
 * @brief Initialise the OTA manager.
 *
 * Must be called after nvs_flash_init() and esp_netif_init().
 * Validates that the partition table contains ota_0, ota_1, and otadata.
 * If OTA_ROLLBACK_ENABLED=y and OTA_HEALTHCHECK_TIMEOUT_SEC>0, arms the
 * healthcheck timer when the running image is PENDING_VERIFY.
 *
 * @param cfg  Config struct. server_url is NULL-safe.
 * @return ESP_OK on success.
 *         ESP_ERR_NOT_SUPPORTED if the partition table is incompatible.
 *         ESP_ERR_NO_MEM if a mutex or timer could not be allocated.
 *         ESP_ERR_INVALID_ARG if cfg is NULL.
 */
esp_err_t ota_manager_init(const ota_manager_config_t *cfg);

/**
 * @brief Start the background OTA task.
 *
 * In poll mode, spawns the FreeRTOS poll task. In manual mode, this is a
 * no-op that logs and returns ESP_OK. Must be called after ota_manager_init().
 *
 * @return ESP_OK on success (including the manual-mode no-op).
 *         ESP_ERR_NO_MEM if the task could not be created.
 */
esp_err_t ota_manager_start_task(void);

/**
 * @brief Manually trigger an OTA check and update.
 *
 * Blocks until the operation completes. Safe to call from any task.
 * Available in both poll and manual modes.
 *
 * On a successful apply:
 *   - if CONFIG_OTA_AUTO_REBOOT=y, the device reboots and this call does
 *     not return;
 *   - if CONFIG_OTA_AUTO_REBOOT=n, fires OTA_EVENT_SUCCESS and returns
 *     ESP_OK; the application is responsible for rebooting.
 *
 * @return ESP_OK if an update was applied (see reboot behaviour above).
 *         ESP_ERR_NOT_FOUND if the server has no newer firmware.
 *         ESP_ERR_INVALID_STATE if an OTA is already in progress.
 *         ESP_ERR_INVALID_ARG if no server URL is configured.
 *         Other esp_err_t on transport, fetch, or write failure.
 */
esp_err_t ota_manager_check_and_update(void);

/**
 * @brief Confirm the running app is healthy.
 *
 * Call this once the application has proven itself healthy after a boot —
 * for example after the network is up and core subsystems are running, NOT
 * merely because app_main reached a line. Marks the image valid, cancels
 * pending rollback, and stops the healthcheck timer.
 *
 * If OTA_HEALTHCHECK_TIMEOUT_SEC>0 and this is not called in time, the
 * component reboots into the previous image. No-op if OTA_ROLLBACK_ENABLED=n
 * or the running image is not pending verification.
 */
void ota_manager_confirm_valid(void);

/**
 * @brief Trigger rollback to the previous OTA image and reboot.
 *
 * Fires OTA_EVENT_ROLLBACK_TRIGGERED (best-effort) then reboots into the
 * previous image. No-op if OTA_ROLLBACK_ENABLED=n or no previous image exists.
 */
void ota_manager_rollback(void);

/**
 * @brief Return the currently running app version string.
 *
 * Reads from esp_app_get_description(). Never returns NULL.
 */
const char *ota_manager_current_version(void);

/**
 * @brief Return the current OTA manager state.
 */
ota_manager_state_t ota_manager_get_state(void);

#ifdef __cplusplus
}
#endif

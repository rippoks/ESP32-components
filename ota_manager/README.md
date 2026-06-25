# ota_manager

ESP-IDF component for over-the-air firmware updates with optional version checking, rollback, and poll/manual trigger modes. Supports ESP32, ESP32-S3, and ESP32-C3 on ESP-IDF 5.x (5.1 minimum).

---

## Prerequisites

**Secure HTTPS (recommended for production)**
Add to your project's `sdkconfig` (or `sdkconfig.defaults`):

```
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
```

This enables the embedded CA certificate bundle required for TLS server verification. Without it, HTTPS connections will fail unless `OTA_HTTPS_INSECURE=y` is set (development only — see Security Note).

**Rollback support**
No manual sdkconfig change is needed. When `CONFIG_OTA_ROLLBACK_ENABLED=y` (the default), the Kconfig `select` directive automatically enables `BOOTLOADER_APP_ROLLBACK_ENABLE`. The bootloader is rebuilt with rollback support the next time you run `idf.py build`.

**Partition table**
A two-OTA partition table is required. The component validates the presence of `ota_0`, `ota_1`, and `otadata` at init and returns `ESP_ERR_NOT_SUPPORTED` if they are absent. See the Partition Table section below.

---

## Integration

Add `ota_manager` as a dependency in your consuming project's `main/idf_component.yml`:

```yaml
dependencies:
  ota_manager:
    path: "D:/projects/ESP32-components/ota_manager"
```

For shared repositories and CI environments, prefer a path relative to the consuming project or a git reference so the dependency resolves on any machine without hardcoded local paths:

```yaml
dependencies:
  ota_manager:
    path: "../../ESP32-components/ota_manager"
    # or:
    # git: "https://github.com/your-org/ESP32-components.git"
    # version: "v0.2.1"
```

---

## Partition Table

The minimum required partition layout. Save it as `partitions.csv` in your project root and enable the custom partition table in `sdkconfig.defaults` (or via `idf.py menuconfig` → *Partition Table*):

```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

```csv
# Name,   Type, SubType,  Offset,   Size,     Flags
nvs,      data, nvs,      0x9000,   0x6000,
otadata,  data, ota,      0xf000,   0x2000,
ota_0,    app,  ota_0,    0x20000,  0x180000,
ota_1,    app,  ota_1,    0x1A0000, 0x180000,
```

No factory partition: the first image is flashed to `ota_0` and the bootloader boots it by default. Adjust slot sizes to your flash capacity. For 4 MB flash, two 1.5 MB OTA slots fit alongside bootloader, partition table, NVS, and otadata, leaving the top of flash unallocated.

---

## Kconfig Reference

All options live under `menu "OTA Manager"` in `Kconfig`. Every symbol is prefixed `CONFIG_OTA_` in C and sdkconfig.

| Symbol | Type | Default | Description |
|---|---|---|---|
| `OTA_TRANSPORT_HTTPS` | bool | y | Use HTTPS transport. If n, falls back to plain HTTP. |
| `OTA_SERVER_URL` | string | `""` | Default firmware binary URL. Overridable at runtime via config struct. Must be non-empty by check time. |
| `OTA_VERSION_CHECK` | bool | y | Reject update unless remote version is strictly newer than the running app. |
| `OTA_ROLLBACK_ENABLED` | bool | y | Enable rollback on boot failure. Requires two OTA partitions. `select`s `BOOTLOADER_APP_ROLLBACK_ENABLE`. |
| `OTA_HEALTHCHECK_TIMEOUT_SEC` | int | 0 | If >0 and rollback enabled: seconds after boot within which the app must call `ota_manager_confirm_valid()`, else auto-rollback + reboot. 0 disables (app fully responsible). Range 0–3600. Visible only if `OTA_ROLLBACK_ENABLED=y`. |
| `OTA_TRIGGER_MODE` (choice) | choice | `OTA_TRIGGER_POLL` | Selects how updates are initiated. Exactly one of the two below. |
| ↳ `OTA_TRIGGER_POLL` | bool | y | Background task polls the server on a timer. |
| ↳ `OTA_TRIGGER_MANUAL` | bool | n | No poll task. App drives updates via `ota_manager_check_and_update()`. |
| `OTA_POLL_INTERVAL_SEC` | int | 3600 | Poll interval in seconds. Range 60–86400. Visible only if `OTA_TRIGGER_POLL=y`. |
| `OTA_POLL_JITTER_PCT` | int | 10 | Random jitter added to each poll interval, as a percentage of the interval. Range 0–50. Visible only if `OTA_TRIGGER_POLL=y`. |
| `OTA_AUTO_REBOOT` | bool | y | Reboot automatically after a successful apply in `ota_manager_check_and_update`. If n, the app reboots when ready. (Poll mode always reboots regardless — see Reboot Behaviour.) |
| `OTA_PROGRESS_CALLBACK` | bool | y | Enable progress/event callback registration. |
| `OTA_TASK_STACK_SIZE` | int | 8192 | OTA background task stack size in bytes. |
| `OTA_TASK_PRIORITY` | int | 5 | OTA background task FreeRTOS priority. |
| `OTA_HTTPS_INSECURE` | bool | n | **Development only.** Disable TLS server authentication entirely (no CA verification, no hostname check). Visible only if `OTA_TRANSPORT_HTTPS=y`. Never enable in production. |

---

## Application Usage

The public API is declared in `include/ota_manager.h`. Functions:

- `ota_manager_init(cfg)` — initialise; validates partition table, arms healthcheck timer if applicable
- `ota_manager_start_task()` — spawn poll task (poll mode) or no-op (manual mode)
- `ota_manager_check_and_update()` — manually trigger a blocking OTA check and update
- `ota_manager_confirm_valid()` — mark the running image healthy; cancels rollback and stops the healthcheck timer
- `ota_manager_rollback()` — trigger immediate rollback to the previous image
- `ota_manager_current_version()` — return the running app version string (never NULL)
- `ota_manager_get_state()` — return the current `ota_manager_state_t` (`OTA_MGR_STATE_IDLE`, `OTA_MGR_STATE_CHECKING`, `OTA_MGR_STATE_DOWNLOADING`, `OTA_MGR_STATE_APPLYING`)

The event callback receives `ota_event_data_t` with one of these events: `OTA_EVENT_CHECK_START`, `OTA_EVENT_UPDATE_AVAILABLE`, `OTA_EVENT_UP_TO_DATE`, `OTA_EVENT_DOWNLOAD_START`, `OTA_EVENT_PROGRESS`, `OTA_EVENT_DOWNLOAD_DONE`, `OTA_EVENT_APPLYING`, `OTA_EVENT_SUCCESS`, `OTA_EVENT_FAILED`, `OTA_EVENT_ROLLBACK_TRIGGERED`. The `data.progress_pct` field (0–100) is valid on `OTA_EVENT_PROGRESS`; `data.err` is valid on `OTA_EVENT_FAILED`. See the header for the full struct definitions and per-function return codes.

Minimal `app_main` for poll mode with rollback enabled:

```c
#include "ota_manager.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

static void ota_cb(const ota_event_data_t *ev, void *ctx) {
    if (ev->event == OTA_EVENT_PROGRESS)
        ESP_LOGI("APP", "OTA progress: %d%%", ev->data.progress_pct);
    if (ev->event == OTA_EVENT_FAILED)
        ESP_LOGE("APP", "OTA failed: %s", esp_err_to_name(ev->data.err));
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ... bring up Wi-Fi and block until connected ...

    ota_manager_config_t cfg = {
        .server_url        = NULL,   // use CONFIG_OTA_SERVER_URL
        .event_cb          = ota_cb,
        .event_cb_ctx      = NULL,
        .poll_interval_sec = 0,      // use CONFIG_OTA_POLL_INTERVAL_SEC
    };
    ESP_ERROR_CHECK(ota_manager_init(&cfg));

    // The image is healthy only once the app has proven itself: network up,
    // core subsystems running. Call confirm_valid HERE, not immediately after
    // init. With OTA_HEALTHCHECK_TIMEOUT_SEC > 0, failing to reach this line in
    // time triggers an automatic rollback on the next boot.
    ota_manager_confirm_valid();

    ESP_ERROR_CHECK(ota_manager_start_task());
}
```

Gate `ota_manager_confirm_valid()` on the specific subsystem coming online — broker connected, panel link established, first sensor reading taken — rather than on `app_main` merely advancing a few lines. That is the difference between rollback protecting you and rollback being decorative.

---

## Rollback Contract

When `OTA_ROLLBACK_ENABLED=y`, the application must call `ota_manager_confirm_valid()` exactly once per boot after the app has demonstrated that it is healthy (network up, core subsystems running). A newly applied image boots in `PENDING_VERIFY` state; `confirm_valid()` stops the healthcheck timer (if armed) and calls `esp_ota_mark_app_valid_cancel_rollback()`, permanently marking the image good. If `OTA_HEALTHCHECK_TIMEOUT_SEC` is set to a value greater than zero, the component arms a one-shot timer at init: if `confirm_valid()` is not called within that window, the timer fires `esp_ota_mark_app_invalid_rollback_and_reboot()`, forcing a reboot into the previous image. When `OTA_HEALTHCHECK_TIMEOUT_SEC=0` (the default), no timer is armed and reversion happens only on the next boot after a crash while the image is still pending — you must call `confirm_valid()` yourself or a silent crash will roll back on the following reboot. Note the healthcheck-race caveat: `confirm_valid()` and the timer callback can fire near-simultaneously, so a device can revert despite a confirm issued a moment too late. The timer is a backstop, not a deadline — set `OTA_HEALTHCHECK_TIMEOUT_SEC` generously (well beyond worst-case healthy-boot time) so the race window is negligible. Do not use it as a tight liveness check.

---

## Reboot Behaviour

`OTA_AUTO_REBOOT=y` (the default) causes the device to reboot immediately inside `ota_manager_check_and_update()` after a successful apply, so that call never returns on success. With `OTA_AUTO_REBOOT=n`, `check_and_update()` instead fires `OTA_EVENT_SUCCESS` and returns `ESP_OK`, leaving the reboot timing to the application — useful in manual mode when the app wants to finish a transaction, flush logs, or notify a server before rebooting. In poll mode, `OTA_AUTO_REBOOT=n` is unsupported: after `esp_ota_set_boot_partition()` the new image is selected but not yet running, so a poll loop that does not reboot will re-download the same firmware on the very next interval and loop indefinitely. The poll task therefore always calls `esp_restart()` on a successful apply regardless of this flag. Keep `OTA_AUTO_REBOOT=y` when using poll mode; use `n` only in manual mode where the application controls the reboot cadence.

---

## Version Endpoint

The component derives the version endpoint URL by appending `.version` to the firmware binary URL (e.g. `https://example.com/fw/app.bin` → `https://example.com/fw/app.bin.version`). The endpoint must return a plain-text `X.Y.Z` semver string, optionally prefixed with `v` or `V` (e.g. `1.2.3` or `v1.2.3`). Trailing pre-release or build-metadata suffixes are ignored, so git-describe strings such as `1.2.3-4-gabc1234` compare on their release part. Comparison is numeric per field. The consuming project must set `PROJECT_VER` (or `version.txt`) to a clean `X.Y.Z` string; a purely non-numeric version tag will be rejected by the comparator and no update will ever be applied. The version fetch uses the same transport security as the firmware download: with `OTA_TRANSPORT_HTTPS=y` (and `OTA_HTTPS_INSECURE=n`) the `.version` request is made over HTTPS with the CA bundle attached, so an HTTPS firmware URL works end to end. There are two distinct failure modes: a fetch failure (no HTTP 200, transport error) fires `OTA_EVENT_FAILED` and returns an error code; a successful fetch whose body cannot be parsed as a version is treated as up-to-date (`OTA_EVENT_UP_TO_DATE`, `ESP_ERR_NOT_FOUND`, logged at WARN). Garbage on the wire therefore never triggers an update and never masquerades as a transport failure.

---

## Security Note

> **Warning: development-only flags must never be enabled in production firmware.**

`OTA_HTTPS_INSECURE=y` disables TLS server authentication entirely — no CA verification, no hostname check. The plain-HTTP transport (`OTA_TRANSPORT_HTTPS=n`) provides no transport security at all. Either path allows a network attacker to serve arbitrary firmware to the device. There is no image signing in v1; the component does not verify the cryptographic authenticity of the downloaded binary beyond the ESP-IDF image descriptor check. Image signing (secure boot / RSA/ECDSA) is the first v2 candidate. For production, use HTTPS with `OTA_HTTPS_INSECURE=n` and `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` so the server certificate is verified against the embedded CA bundle. In secure mode the HTTPS transport attaches the bundle via `esp_crt_bundle_attach`; v1 does not pass a per-request `cert_pem`, so serving firmware from a host whose CA is outside the bundle requires customizing the bundle itself (`CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`).

---

## Unit Test

A host-compiled unit test for the version comparator lives at `test/test_ota_version.c`. It does not require an IDF environment; build it with `-DUNIT_TEST` against `src/ota_version.c` using any desktop C compiler.

```bash
gcc -DUNIT_TEST -I include src/ota_version.c test/test_ota_version.c -o test_ota_version && ./test_ota_version
```

# ota_manager — Component Specification (v0.2.1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a reusable, Kconfig-configurable ESP-IDF 5.x OTA manager component that can be dropped into any ESP32 project via a local path reference in `idf_component.yml`.

**Architecture:** Single component directory at `D:/projects/ESP32-components/ota_manager/`. Public API exposed via one header. All feature selection is compile-time via Kconfig. Runtime config struct allows per-project overrides of selected Kconfig defaults. No external dependencies beyond ESP-IDF 5.x built-ins.

**Tech Stack:** C (C17), ESP-IDF 5.x, FreeRTOS, `esp_ota_ops`, `esp_https_ota`, `esp_http_client`, `esp_tls`, `esp_app_format`, `esp_timer`, `nvs_flash`, `esp_log`

---

## Changes in v0.2.1 (corrections to v0.2.0)

A deep self-review of v0.2.0 found that several of its own fixes introduced new inconsistencies, concentrated in event firing and the state machine. This patch level corrects them. Read this first if coming from v0.2.0.

1. **Event firing is centralized; `OTA_EVENT_UPDATE_AVAILABLE` fires exactly once.** v0.2.0 fired it in `ota_manager.c` *and* in both transports (double-fire on the HTTP path, incoherent sequencing on the HTTPS path). It now fires once, centrally, in `ota_manager.c`, and never in a transport. A single `ota_fire()` helper is the only event-emitting mechanism; both `ota_manager.c` and the trampoline call it.
2. **The HTTPS img-desc mismatch now fails instead of reporting up-to-date.** A `.version` file that disagrees with the embedded image version is a server error, so the guard fires `OTA_EVENT_FAILED` (with `ESP_ERR_INVALID_VERSION`) and aborts, rather than firing `UP_TO_DATE`.
3. **`OTA_MGR_STATE_FAILED` removed from the public state enum.** It was unobservable (reset to `IDLE` on return) and the alternative wedged the poll loop on `INVALID_STATE` after one failure. Failure is reported only via `OTA_EVENT_FAILED` and the return code. The observable in-flight states (`IDLE`/`CHECKING`/`DOWNLOADING`/`APPLYING`) remain.
4. **Module-level statics are now explicitly declared with correct guards.** `s_transport_ops` and `s_healthcheck_timer` were referenced but declared in no task. `s_healthcheck_timer` is declared under `#if CONFIG_OTA_ROLLBACK_ENABLED` (not the narrower timeout guard), because `confirm_valid` references it even when the timeout is 0.
5. **Download buffer moved off the task stack.** The HTTP transport's 4 KB buffer is now heap-allocated, so it does not consume half of `OTA_TASK_STACK_SIZE`.
6. **Smoke test now manages version numbers across steps.** Each apply uses a bumped `.version` (or disables the version check for that step), so the rollback-trigger step actually has an update to apply.
7. **`esp_hw_support` added to `REQUIRES`** for `esp_random()` used by poll jitter.
8. **Garbage-but-fetched version is defined behaviour.** A successful fetch whose body does not parse is treated as not-newer (logged `WARN`), distinct from a fetch *failure* which fires `OTA_EVENT_FAILED`.
9. **Healthcheck timer race acknowledged.** A near-simultaneous `confirm_valid()` and timer expiry can revert despite a just-issued confirm; the timer is a backstop, not a deadline, so the timeout must be generous enough to make the window negligible.
10. **Minor:** secure HTTPS branch includes `esp_crt_bundle.h`; the `esp_timer` healthcheck callback must use default task dispatch (never ISR dispatch — it calls flash operations).

---

## Changes in v0.2.0

This revision corrects design and correctness issues found in review of v0.1.0. Implementers coming from v0.1.0 should read this list first.

**Behavioural / contract fixes**

1. **Reboot is no longer unconditional inside `ota_manager_check_and_update`.** A new `CONFIG_OTA_AUTO_REBOOT` (default `y`) governs whether the component reboots after a successful apply. When off, the function fires `OTA_EVENT_SUCCESS`, returns `ESP_OK`, and the application decides when to reboot (graceful shutdown, flush, notify server). The poll task still always reboots after a successful apply, so `OTA_AUTO_REBOOT=n` is only meaningful in manual mode.
2. **Rollback is now genuinely protective, not a boot-time formality.** New `CONFIG_OTA_HEALTHCHECK_TIMEOUT_SEC` (default `0` = disabled). When set and rollback is enabled, the component arms a one-shot timer at boot; if the app does not call `ota_manager_confirm_valid()` within the window, the component invalidates the image and reboots into the previous one. The minimal example now calls `confirm_valid()` **after** the app is demonstrably healthy (network up), not immediately after init.
3. **Rollback prerequisite made explicit.** `OTA_ROLLBACK_ENABLED` now `select`s `BOOTLOADER_APP_ROLLBACK_ENABLE`. Without that bootloader option the rollback APIs are inert; v0.1.0 did not state this.
4. **Rollback timing description corrected.** There is no IDF timer that auto-reverts a running, unconfirmed image. Reversion happens on the *next boot* if the image is still `PENDING_VERIFY` (e.g. after a crash/brownout/WDT reboot). The optional healthcheck timer above is what forces that reboot if you want time-bounded protection.

**Correctness fixes**

5. **HTTP transport now opens the connection and reads headers** (`esp_http_client_open` + `esp_http_client_fetch_headers`) before the read loop. Content length comes from `fetch_headers` / `esp_http_client_get_content_length`. v0.1.0's read loop would not have worked as written.
6. **Version parsing hardened.** Tolerates an optional leading `v`/`V`, parses the `MAJOR.MINOR.PATCH` numeric prefix (so git-describe strings like `1.2.3-4-gabc` compare correctly), and ignores pre-release/build suffixes. Comparison is numeric per field, not lexical. A host-side unit test now covers the edge cases.
7. **Version-fetch failure is no longer silently treated as up-to-date.** A failed `.version` fetch fires `OTA_EVENT_FAILED` and returns the error.
8. **Insecure TLS flag renamed and corrected.** `OTA_HTTPS_SKIP_CERT_VERIFY` → `OTA_HTTPS_INSECURE`. `skip_cert_common_name_check` only skips the hostname check; the spec now documents what actually disables chain verification and warns that the exact behaviour is IDF-version-dependent.
9. **State machine is now wired.** `DOWNLOADING`/`APPLYING` states are set during the flow via an internal event trampoline, and a public `ota_manager_get_state()` getter exposes them. (v0.2.1 note: an earlier `FAILED` state was removed — see the v0.2.1 changelog.)
10. **All declared events now fire.** `OTA_EVENT_UPDATE_AVAILABLE` fires after a passed version check; `OTA_EVENT_ROLLBACK_TRIGGERED` fires (best-effort) before the rollback reboot.
11. **`ota_manager_start_task` returns `esp_err_t`** instead of `void`, and init no longer `assert`s on resource-allocation failure — a reusable component must not abort the host app. Both return errors instead.

**Robustness / fleet**

12. **Trigger mode is a Kconfig `choice`** (`poll` / `manual`) rather than two mutually-dependent bools.
13. **Poll jitter** (`CONFIG_OTA_POLL_JITTER_PCT`, default 10) plus an initial randomized delay, so a fleet booting together does not stampede the update server.
14. **Runtime `poll_interval_sec` override is clamped** to the supported range.
15. **HTTPS path validates the embedded image descriptor** (`esp_https_ota_get_img_desc`) as a second-line guard, so a lying `.version` file cannot cause a same/older image to be applied.

`OTA_HTTPS_INSECURE`, the plain-HTTP transport, and unsigned images together remain the largest risk surface. Image signing stays out of scope for v1 but is the first v2 candidate; the README must fence the dev-only flags loudly.

---

## Target Environment

- **Component root:** `D:/projects/ESP32-components/ota_manager/`
- **ESP-IDF version:** 5.x (5.1 minimum). Pin one minor version per project; the insecure-TLS path in particular is version-sensitive.
- **Target chips:** ESP32, ESP32-S3, ESP32-C3 (no chip-specific code — use IDF abstractions only)
- **Framework:** ESP-IDF native (not Arduino)
- **Build system:** CMake via `idf.py`
- **Consuming project integration:** `idf_component.yml` local path reference
- **Required consumer sdkconfig (secure HTTPS):** `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`
- **Required consumer sdkconfig (rollback):** satisfied automatically — `OTA_ROLLBACK_ENABLED` `select`s `BOOTLOADER_APP_ROLLBACK_ENABLE`

---

## Directory Structure

```
ota_manager/
  CMakeLists.txt              # Component registration, conditional source inclusion
  Kconfig                     # All compile-time options with defaults
  idf_component.yml           # Component metadata (name, version, description)
  include/
    ota_manager.h             # Public API — structs, enums, function signatures, callback typedef
  src/
    ota_manager.c             # Core init, task, state machine, partition validation, rollback, healthcheck timer
    ota_transport_https.c     # HTTPS transport (conditionally compiled)
    ota_transport_http.c      # HTTP transport (conditionally compiled)
    ota_version.c             # Version comparison + fetch (conditionally compiled)
  test/
    test_ota_version.c        # Host-compiled unit test for the version comparator (no IDF required)
  README.md                   # Integration guide: prerequisites, idf_component.yml snippet, Kconfig options, API usage
```

---

## Kconfig Options

All options live under `menu "OTA Manager"` in `Kconfig`. Prefix: `CONFIG_OTA_`.

| Kconfig symbol | Type | Default | Description |
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
| `OTA_AUTO_REBOOT` | bool | y | Reboot automatically after a successful apply in `ota_manager_check_and_update`. If n, the app reboots when ready. (Poll mode always reboots regardless — see note.) |
| `OTA_PROGRESS_CALLBACK` | bool | y | Enable progress/event callback registration. |
| `OTA_TASK_STACK_SIZE` | int | 8192 | OTA background task stack size in bytes. |
| `OTA_TASK_PRIORITY` | int | 5 | OTA background task FreeRTOS priority. |
| `OTA_HTTPS_INSECURE` | bool | n | **Development only.** Disable TLS server authentication entirely (no CA verification, no hostname check). Visible only if `OTA_TRANSPORT_HTTPS=y`. Never enable in production. |

**Mutual exclusion** between poll and manual triggers is expressed with a Kconfig `choice` (not two `depends on` bools). `OTA_POLL_INTERVAL_SEC` and `OTA_POLL_JITTER_PCT` `depends on OTA_TRIGGER_POLL`.

**`OTA_AUTO_REBOOT=n` + poll mode is unsupported.** After `esp_ota_set_boot_partition` the new image is selected but not yet booted; a poll loop that does not reboot will keep re-downloading. The poll task therefore reboots on a successful apply regardless of this flag. Keep `OTA_AUTO_REBOOT=y` in poll mode; use `n` only in manual mode where the app controls cadence.

---

## Public API — `include/ota_manager.h`

```c
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
```

---

## `CMakeLists.txt`

```cmake
idf_component_register(
    SRCS
        "src/ota_manager.c"
        $<$<BOOL:${CONFIG_OTA_TRANSPORT_HTTPS}>:src/ota_transport_https.c>
        $<$<BOOL:${CONFIG_OTA_VERSION_CHECK}>:src/ota_version.c>
        $<$<NOT:$<BOOL:${CONFIG_OTA_TRANSPORT_HTTPS}>>:src/ota_transport_http.c>
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        esp_https_ota
        esp_partition
        nvs_flash
        app_update
        esp_app_format
        esp_hw_support
        freertos
        esp_timer
        log
)
```

`esp_app_format` is required for `esp_app_get_description`. `esp_timer` is required for the optional healthcheck timer. `esp_hw_support` is required for `esp_random` (poll jitter). `esp-tls` and the certificate bundle are pulled transitively by `esp_https_ota` when the HTTPS transport is compiled; secure operation additionally requires `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` in the consuming project.

---

## `idf_component.yml`

```yaml
version: "0.2.1"
description: "Configurable drop-in OTA manager for ESP-IDF 5.x"
# url is informational only. Consuming projects reference this component by
# local path (see README). For portability across machines/CI, prefer a
# relative path or a git reference over a machine-specific absolute path.
url: "https://github.com/<owner>/ESP32-components"
```

---

## Internal Design Contracts

### Partition Validation (enforced in `ota_manager_init`)

`ota_manager_init` must call `esp_partition_find_first()` for all three of:
- `ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0`
- `ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1`
- `ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA`

If any are NULL, return `ESP_ERR_NOT_SUPPORTED` and log an error naming the missing partition. Do not proceed.

### State Machine

`ota_manager.c` maintains a module-level `ota_manager_state_t s_state` (the same enum exposed publicly). The complete set of module statics, with their guards, is:

```c
static ota_manager_state_t s_state = OTA_MGR_STATE_IDLE;
static SemaphoreHandle_t   s_mutex = NULL;
static ota_manager_config_t s_cfg;
static ota_transport_ops_t  s_transport_ops;
#if CONFIG_OTA_ROLLBACK_ENABLED
static esp_timer_handle_t   s_healthcheck_timer = NULL;  // guarded by ROLLBACK, not the timeout —
                                                         // confirm_valid() references it even when timeout=0
#endif
```

State transitions are guarded by `s_mutex`. A call to `ota_manager_check_and_update()` while `s_state != OTA_MGR_STATE_IDLE` returns `ESP_ERR_INVALID_STATE` immediately without blocking. `ota_manager_get_state()` reads `s_state` under the mutex. There is deliberately no public `FAILED` state: a failure is reported by `OTA_EVENT_FAILED` plus the return code, and `s_state` returns to `IDLE` so the next check (or the poll loop) is not wedged.

**Event delivery has exactly one mechanism — `ota_fire()` — which is the only place the app callback is invoked:**

```c
static void ota_fire(const ota_event_data_t *ev) {
    if (s_cfg.event_cb) {
        s_cfg.event_cb(ev, s_cfg.event_cb_ctx);
    }
}

static void ota_fire_evt(ota_event_t evt) {            // payload-less events
    ota_event_data_t ev = { .event = evt };
    ota_fire(&ev);
}
```

`ota_manager.c` fires its own events (`CHECK_START`, `UPDATE_AVAILABLE`, `UP_TO_DATE`, `ROLLBACK_TRIGGERED`, and `FAILED` on version-fetch errors) by calling `ota_fire`/`ota_fire_evt` directly. **Transports never fire `UPDATE_AVAILABLE` or `UP_TO_DATE`** — those belong to the core so they fire exactly once.

States are advanced by an **internal event trampoline** that wraps `ota_fire`, so the transport stays decoupled from state management:

```c
// Registered with the transport in place of the app callback. Adds state
// tracking, then delivers via ota_fire. cb_ctx is unused (ota_fire reads
// s_cfg directly); transports are always invoked as
// perform(url, ota_internal_event, NULL).
static void ota_internal_event(const ota_event_data_t *ev, void *unused) {
    switch (ev->event) {
        case OTA_EVENT_DOWNLOAD_START: ota_set_state(OTA_MGR_STATE_DOWNLOADING); break;
        case OTA_EVENT_APPLYING:       ota_set_state(OTA_MGR_STATE_APPLYING);    break;
        default: break;  // FAILED is handled by the core resetting state to IDLE
    }
    ota_fire(ev);
}
```

`ota_set_state()` is a small helper that takes the mutex, writes `s_state`, releases.

### Transport Interface (internal, not public)

```c
typedef struct {
    esp_err_t (*perform)(const char *url, ota_event_cb_t cb, void *cb_ctx);
} ota_transport_ops_t;
```

`ota_transport_https.c` and `ota_transport_http.c` each export one function matching this signature. `ota_manager.c` selects the implementation at compile time via `#if CONFIG_OTA_TRANSPORT_HTTPS`.

A transport fires an event by invoking its supplied `cb(&ev, cb_ctx)` — in practice the core's `ota_internal_event` trampoline, which updates state then delivers via `ota_fire`. A transport never calls `s_cfg.event_cb` directly, and never fires `UPDATE_AVAILABLE` or `UP_TO_DATE` (those are the core's). A transport owns `DOWNLOAD_START`, `PROGRESS`, `DOWNLOAD_DONE`, `APPLYING`, `SUCCESS`, and `FAILED`.

### HTTPS Transport (`ota_transport_https.c`)

Uses the advanced handle API (`esp_https_ota_begin` / `esp_https_ota_perform` / `esp_https_ota_finish`), not the one-shot helper, so progress can be reported.

Config (the secure branch requires `#include "esp_crt_bundle.h"`):
```c
esp_http_client_config_t http_config = {
    .url        = url,
    .timeout_ms = 30000,
};
#if CONFIG_OTA_HTTPS_INSECURE
    http_config.crt_bundle_attach          = NULL;
    http_config.cert_pem                   = NULL;
    http_config.skip_cert_common_name_check = true;
#else
    http_config.crt_bundle_attach          = esp_crt_bundle_attach;
#endif
```

**Insecure mode caveat.** `skip_cert_common_name_check` disables only the hostname/CN match, not chain-of-trust. Omitting the CA bundle removes the trust anchor, but whether the handshake then proceeds unauthenticated is esp-tls/IDF-version-dependent. The implementer must verify on the pinned IDF version that `OTA_HTTPS_INSECURE=y` actually completes an unverified handshake (and that `=n` rejects an untrusted cert). Log `ESP_LOGW("OTA_MGR", "INSECURE TLS: server certificate is NOT verified")` on every check when this is active.

Second-line version guard (only when `CONFIG_OTA_VERSION_CHECK=y`): after `esp_https_ota_begin`, call `esp_https_ota_get_img_desc(handle, &img_desc)` and compare `img_desc.version` against `ota_manager_current_version()` via `ota_version_is_newer`. The core's `.version` pre-fetch already decided an update exists, so if the *embedded* image version is not strictly newer the two disagree — that is a server/upload error, not an up-to-date condition. On mismatch: `esp_https_ota_abort`, fire `OTA_EVENT_FAILED` with `ESP_ERR_INVALID_VERSION`, and return that error. On match, fire no event (the core already fired `OTA_EVENT_UPDATE_AVAILABLE` before calling the transport).

Progress:
```c
int image_size   = esp_https_ota_get_image_size(handle);
int read_so_far  = esp_https_ota_get_image_len_read(handle);
uint8_t pct = (image_size > 0) ? (uint8_t)((read_so_far * 100) / image_size) : 0;
```
Fire `OTA_EVENT_PROGRESS` only when `pct` crosses a 10% boundary from the last reported value (never every chunk).

### HTTP Transport (`ota_transport_http.c`)

Plain HTTP fallback using `esp_http_client` directly with `esp_ota_ops` (`esp_ota_begin` / `esp_ota_write` / `esp_ota_end`). Does not use `esp_https_ota`.

The connection **must** be opened and headers fetched before reading:
```c
esp_http_client_open(client, 0);
int64_t content_len = esp_http_client_fetch_headers(client); // also via esp_http_client_get_content_length()
```
`content_len` drives the progress denominator; if it is <= 0, report `0` throughout. Begin the OTA on the next update partition (`esp_ota_get_next_update_partition(NULL)`) with `OTA_WITH_SEQUENTIAL_WRITES` (size `OTA_SIZE_UNKNOWN` when content length is unknown). Progress reporting is identical to the HTTPS transport (10% increments).

### Version Check (`ota_version.c`)

Only compiled if `CONFIG_OTA_VERSION_CHECK=y`.

Server exposes a version endpoint: the firmware binary URL with `.version` appended, returning a plain-text semver string (e.g. `1.2.3`).

Parsing tolerates an optional leading `v`/`V` and reads the numeric `MAJOR.MINOR.PATCH` prefix; trailing pre-release/build suffixes are ignored. This makes git-describe strings (`1.2.3-4-gabc`) and `v`-prefixed tags compare on their release version. Comparison is numeric per field. If either string lacks a parseable `X.Y.Z` prefix, comparison returns `false` and logs `ESP_LOGW`.

The core distinguishes two failure modes so callers can tell a broken server from a broken version string:
- A **fetch failure** (no HTTP 200, transport error) fires `OTA_EVENT_FAILED` and returns the error.
- A **successful fetch whose body does not parse** yields `is_newer == false`, which the core treats as up-to-date (`OTA_EVENT_UP_TO_DATE`, `ESP_ERR_NOT_FOUND`), logged at `WARN`.

Garbage on the wire therefore never triggers an update and never masquerades as a transport failure.

> **Project requirement:** consuming projects must set `PROJECT_VER` (or the version line in `version.txt`) to a clean `X.Y.Z` (optionally `v`-prefixed). A purely non-numeric version tag will be rejected by the comparator and no update will ever be applied.

### Rollback Wiring

Effective only when `CONFIG_OTA_ROLLBACK_ENABLED=y`, which `select`s `BOOTLOADER_APP_ROLLBACK_ENABLE`.

- `ota_manager_init` reads `esp_ota_get_state_partition()` on the running partition. If `ESP_OTA_IMG_PENDING_VERIFY`, it logs a warning and, if `OTA_HEALTHCHECK_TIMEOUT_SEC>0`, arms a one-shot `esp_timer` for that many seconds.
- `ota_manager_confirm_valid()` stops and deletes the healthcheck timer (if armed) and calls `esp_ota_mark_app_valid_cancel_rollback()`. The return value is logged but not fatal (it errors if the image was not pending — that is a benign no-op).
- The healthcheck timer callback logs `ESP_LOGE` and calls `esp_ota_mark_app_invalid_rollback_and_reboot()`. Create the timer with default (task) dispatch — **never `ESP_TIMER_ISR`** — because the callback performs flash operations and a reboot, which are illegal from ISR context.
- `ota_manager_rollback()` fires `OTA_EVENT_ROLLBACK_TRIGGERED`, then calls `esp_ota_mark_app_invalid_rollback_and_reboot()`.
- After a successful apply the new image boots `PENDING_VERIFY`. **There is no IDF timer that reverts a running, unconfirmed image.** Reversion happens on the next boot if the image is still pending (e.g. after a crash). The healthcheck timer above is the mechanism that forces that reboot within a bounded window.

> **Healthcheck race.** `ota_manager_confirm_valid()` and the timer callback can fire near-simultaneously, so a device can revert despite a confirm issued a moment too late. The timer is a backstop, not a deadline: set `OTA_HEALTHCHECK_TIMEOUT_SEC` generously (well beyond worst-case healthy-boot time) so the window is negligible. Do not treat it as a tight liveness check.

### Poll Task

Name `"ota_mgr"`, stack `CONFIG_OTA_TASK_STACK_SIZE`, priority `CONFIG_OTA_TASK_PRIORITY`. Uses `vTaskDelay` between checks (no `esp_timer`).

To avoid fleet stampedes:
- Before the **first** check, delay a random `0 .. min(interval, 300)` seconds (`esp_random()`).
- Each subsequent interval is `interval + random(0 .. interval * CONFIG_OTA_POLL_JITTER_PCT / 100)`.

On a successful apply the task reboots: if `OTA_AUTO_REBOOT=y` the reboot already happened inside `check_and_update`; if `=n`, `check_and_update` returns `ESP_OK` and the task calls `esp_restart()` itself.

### Logging

Tag `"OTA_MGR"`. `ESP_LOGI` for normal events, `ESP_LOGW` for degraded conditions (insecure TLS, pending verify, unparseable version, clamped interval), `ESP_LOGE` for failures. No `ESP_LOGD` in v1.

---

## Consuming Project Integration

### `idf_component.yml` (in consuming project's `main/`)

```yaml
dependencies:
  ota_manager:
    path: "D:/projects/ESP32-components/ota_manager"
```

For shared repos/CI, prefer a path relative to the consuming project (e.g. `../../ESP32-components/ota_manager`) or a git reference, so the dependency resolves on any machine.

### Partition Table CSV (minimum required)

```csv
# Name,   Type, SubType,  Offset,   Size,     Flags
nvs,      data, nvs,      0x9000,   0x6000,
otadata,  data, ota,      0xf000,   0x2000,
ota_0,    app,  ota_0,    0x20000,  0x180000,
ota_1,    app,  ota_1,    0x1A0000, 0x180000,
```

No factory partition: the first image is flashed to `ota_0` and the bootloader boots it by default. Adjust sizes to flash capacity. For 4MB flash, two 1.5MB OTA slots fit with room below for bootloader, partition table, NVS, and otadata, leaving the top of flash unallocated.

### Minimal Application Usage

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

For a sensor/panel device, gate `confirm_valid()` on the specific subsystem coming online (broker connected, panel link established, first reading taken) rather than on `app_main` merely advancing — that is the difference between rollback protecting you and rollback being decorative.

---

## Out of Scope for v1

These are explicitly excluded. Do not implement, stub, or prepare hooks for them.

- Signed image verification (secure boot, RSA/ECDSA image signing) — **first v2 candidate**
- Delta/diff OTA
- MQTT-triggered updates
- NVS update history logging
- Version metadata as JSON (plain text `.version` file only)
- Multi-chip or coprocessor OTA
- ESP Component Registry publishing

---

## File-by-File Responsibilities

| File | Responsibility |
|---|---|
| `CMakeLists.txt` | Component registration, conditional source list, REQUIRES list |
| `Kconfig` | All CONFIG_OTA_ symbols, help text, ranges, `choice`, `select` |
| `idf_component.yml` | Component name, version, description |
| `include/ota_manager.h` | All public types, enums, structs, function declarations |
| `src/ota_manager.c` | init, task, state machine + trampoline, mutex, partition validation, rollback + healthcheck timer, URL/interval validation, dispatch to transport ops |
| `src/ota_transport_https.c` | HTTPS transport via advanced esp_https_ota handle API, img-desc guard, progress |
| `src/ota_transport_http.c` | HTTP transport via esp_http_client (open + fetch_headers + read loop) and esp_ota_ops |
| `src/ota_version.c` | Version fetch (HTTP GET to `.version`), tolerant semver parse, numeric comparison |
| `test/test_ota_version.c` | Host-compiled unit test for `ota_version_is_newer` |
| `README.md` | Prerequisites, idf_component.yml snippet, partition CSV, Kconfig reference, usage, rollback contract, version endpoint |

---

## Implementation Plan

### Task 1: Scaffold component directory and build system

**Files:**
- Create: `D:/projects/ESP32-components/ota_manager/CMakeLists.txt`
- Create: `D:/projects/ESP32-components/ota_manager/Kconfig`
- Create: `D:/projects/ESP32-components/ota_manager/idf_component.yml`
- Create: `D:/projects/ESP32-components/ota_manager/include/ota_manager.h`
- Create: `D:/projects/ESP32-components/ota_manager/src/ota_manager.c` (stub)
- Create: `D:/projects/ESP32-components/ota_manager/src/ota_transport_https.c` (stub)
- Create: `D:/projects/ESP32-components/ota_manager/src/ota_transport_http.c` (stub)
- Create: `D:/projects/ESP32-components/ota_manager/src/ota_version.c` (stub)

- [ ] **Step 1: Create directory structure**

```bash
mkdir -p D:/projects/ESP32-components/ota_manager/include
mkdir -p D:/projects/ESP32-components/ota_manager/src
mkdir -p D:/projects/ESP32-components/ota_manager/test
```

- [ ] **Step 2: Write `CMakeLists.txt`**

Exact content as specified in the CMakeLists.txt section above (including `esp_app_format`).

- [ ] **Step 3: Write `Kconfig`**

All symbols from the Kconfig Options table. Use a `choice` named `OTA_TRIGGER_MODE` containing `OTA_TRIGGER_POLL` (default) and `OTA_TRIGGER_MANUAL`. `OTA_ROLLBACK_ENABLED` must `select BOOTLOADER_APP_ROLLBACK_ENABLE`. `OTA_HEALTHCHECK_TIMEOUT_SEC` (`range 0 3600`, `depends on OTA_ROLLBACK_ENABLED`). `OTA_POLL_INTERVAL_SEC` (`range 60 86400`) and `OTA_POLL_JITTER_PCT` (`range 0 50`) both `depends on OTA_TRIGGER_POLL`. `OTA_HTTPS_INSECURE depends on OTA_TRANSPORT_HTTPS`. Include the `OTA_AUTO_REBOOT` help note about poll mode.

- [ ] **Step 4: Write `idf_component.yml`**

Exact content as specified above (`version: "0.2.1"`).

- [ ] **Step 5: Write `include/ota_manager.h`**

Exact content as specified in the Public API section above.

- [ ] **Step 6: Write stub source files**

Each `.c`: include its header, define `static const char *TAG = "OTA_MGR"`. In `ota_manager.c`, define `ota_manager_init` returning `ESP_OK` only, so the component compiles clean before logic is added.

- [ ] **Step 7: Verify component compiles in a test project**

In a scratch ESP32 project with the two-OTA partition CSV and `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`, add to `main/idf_component.yml`:
```yaml
dependencies:
  ota_manager:
    path: "D:/projects/ESP32-components/ota_manager"
```
Run `idf.py build`. Expected: build succeeds with no errors.

- [ ] **Step 8: Commit**

```bash
cd D:/projects/ESP32-components
git init   # if not already a repo
git add ota_manager/
git commit -m "feat: scaffold ota_manager v0.2.1 component structure"
```

---

### Task 2: Version comparator with host unit test (TDD)

**Files:**
- Create: `test/test_ota_version.c`
- Modify: `src/ota_version.c`

The comparator is pure logic with the most edge cases, so it is built test-first and run on the host (no IDF, no hardware).

- [ ] **Step 1: Write the failing test**

`test/test_ota_version.c`: include a forward declaration `bool ota_version_is_newer(const char *remote, const char *local);` and a `main` that asserts:

```c
assert(ota_version_is_newer("1.2.3", "1.2.2") == true);
assert(ota_version_is_newer("1.2.3", "1.2.3") == false);
assert(ota_version_is_newer("1.2.2", "1.2.3") == false);
assert(ota_version_is_newer("v1.2.3", "1.2.3") == false);   // leading v tolerated, equal
assert(ota_version_is_newer("1.2.3-4-gabc", "1.2.2") == true); // suffix ignored
assert(ota_version_is_newer("2.0.0", "1.9.9") == true);
assert(ota_version_is_newer("1.10.0", "1.9.0") == true);    // numeric, not lexical
assert(ota_version_is_newer("garbage", "1.0.0") == false);  // unparseable -> false
assert(ota_version_is_newer("1.0.0", "garbage") == false);
```

Print "ALL PASS" at the end.

- [ ] **Step 2: Run the test to verify it fails to build/link**

Run: `gcc -DUNIT_TEST -I include test/test_ota_version.c src/ota_version.c -o /tmp/ttest && /tmp/ttest`
Expected: link error (function undefined) or assertion failure — the comparator is still a stub.

> `src/ota_version.c` must guard its IDF includes (`esp_http_client.h`, `esp_log.h`) behind `#ifndef UNIT_TEST` so the host build pulls in only the comparator. Use plain `printf`/no-op in place of `ESP_LOGW` under `UNIT_TEST`.

- [ ] **Step 3: Implement the comparator**

```c
static bool parse_semver(const char *s, unsigned *maj, unsigned *min, unsigned *pat) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    return sscanf(s, "%u.%u.%u", maj, min, pat) == 3;
}

bool ota_version_is_newer(const char *remote, const char *local) {
    unsigned rj, rn, rp, lj, ln, lp;
    if (!parse_semver(remote, &rj, &rn, &rp) || !parse_semver(local, &lj, &ln, &lp)) {
        /* ESP_LOGW under firmware build; printf under UNIT_TEST */
        return false;
    }
    if (rj != lj) return rj > lj;
    if (rn != ln) return rn > ln;
    return rp > lp;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `gcc -DUNIT_TEST -I include test/test_ota_version.c src/ota_version.c -o /tmp/ttest && /tmp/ttest`
Expected: prints "ALL PASS", exit 0.

- [ ] **Step 5: Commit**

```bash
git add ota_manager/test/test_ota_version.c ota_manager/src/ota_version.c
git commit -m "test+feat: tolerant semver comparator with host unit test"
```

---

### Task 3: Partition validation, state machine, rollback, and healthcheck timer

**Files:**
- Modify: `src/ota_manager.c`

- [ ] **Step 1: Implement `ota_manager_init` partition check**

If `cfg` is NULL return `ESP_ERR_INVALID_ARG`. Call `esp_partition_find_first` for `ota_0`, `ota_1`, and `otadata`. Return `ESP_ERR_NOT_SUPPORTED` with an `ESP_LOGE` naming the missing partition if any are NULL.

- [ ] **Step 2: Module statics, state helpers, and event delivery (no asserts)**

Declare the full static set with guards:
```c
static ota_manager_state_t  s_state = OTA_MGR_STATE_IDLE;
static SemaphoreHandle_t    s_mutex = NULL;
static ota_manager_config_t s_cfg;
static ota_transport_ops_t  s_transport_ops;
#if CONFIG_OTA_ROLLBACK_ENABLED
static esp_timer_handle_t   s_healthcheck_timer = NULL;  // guarded by ROLLBACK, not the timeout
#endif
```
Create the mutex with `xSemaphoreCreateMutex()`; if NULL, `ESP_LOGE` and return `ESP_ERR_NO_MEM`. Add `static void ota_set_state(ota_manager_state_t s)` (take mutex, write, give) and implement `ota_manager_get_state()` the same way. Add the single event-delivery helpers `ota_fire(const ota_event_data_t *)` and `ota_fire_evt(ota_event_t)` exactly as defined in the State Machine contract — these are the only functions that invoke `s_cfg.event_cb`.

- [ ] **Step 3: Store and validate config**

Declare `static ota_manager_config_t s_cfg;`. Copy `cfg` into `s_cfg`. If `s_cfg.server_url` is NULL set it to `CONFIG_OTA_SERVER_URL`. If the resulting URL is empty, `ESP_LOGW` that no URL is configured (do not fail init — fail at check time). Clamp `poll_interval_sec`: if 0 use `CONFIG_OTA_POLL_INTERVAL_SEC`; else clamp to `[60, 86400]` and `ESP_LOGW` if clamped.

- [ ] **Step 4: Rollback pending check + healthcheck timer**

Guard with `#if CONFIG_OTA_ROLLBACK_ENABLED`. After the partition check, read `esp_ota_get_state_partition()` on `esp_ota_get_running_partition()`. If `ESP_OTA_IMG_PENDING_VERIFY`, `ESP_LOGW("OTA_MGR", "App is PENDING_VERIFY; call ota_manager_confirm_valid() once healthy")`. Then:
```c
#if CONFIG_OTA_HEALTHCHECK_TIMEOUT_SEC > 0
    const esp_timer_create_args_t a = { .callback = ota_healthcheck_cb, .name = "ota_hc" };
    if (esp_timer_create(&a, &s_healthcheck_timer) != ESP_OK) return ESP_ERR_NO_MEM;
    esp_timer_start_once(s_healthcheck_timer,
        (uint64_t)CONFIG_OTA_HEALTHCHECK_TIMEOUT_SEC * 1000000ULL);
#endif
```
`ota_healthcheck_cb`: `ESP_LOGE` then `esp_ota_mark_app_invalid_rollback_and_reboot()`.

- [ ] **Step 5: Implement `ota_manager_current_version`**

```c
const char *ota_manager_current_version(void) {
    return esp_app_get_description()->version;
}
```

- [ ] **Step 6: Implement `ota_manager_confirm_valid` and `ota_manager_rollback`**

Guard with `#if CONFIG_OTA_ROLLBACK_ENABLED` (else compile empty bodies). `confirm_valid`: if `s_healthcheck_timer` is non-NULL, `esp_timer_stop` + `esp_timer_delete` + set it NULL; then `esp_ota_mark_app_valid_cancel_rollback()` (log its return, non-fatal). `rollback`: `ota_fire_evt(OTA_EVENT_ROLLBACK_TRIGGERED)`, then `esp_ota_mark_app_invalid_rollback_and_reboot()`.

- [ ] **Step 7: Verify build still clean**

Run `idf.py build` in the test project with defaults. Expected: no errors.

- [ ] **Step 8: Commit**

```bash
git add ota_manager/src/ota_manager.c
git commit -m "feat: init, partition validation, state machine, rollback + healthcheck timer"
```

---

### Task 4: HTTPS transport

**Files:**
- Modify: `src/ota_transport_https.c`

- [ ] **Step 1: Define transport perform function**

```c
esp_err_t ota_transport_https_perform(const char *url, ota_event_cb_t cb, void *cb_ctx);
```

- [ ] **Step 2: Configure `esp_https_ota_config_t`**

Set `http_config.url = url`, `timeout_ms = 30000`. Apply the `CONFIG_OTA_HTTPS_INSECURE` block from the HTTPS Transport section (and the secure `esp_crt_bundle_attach` otherwise). Emit the insecure `ESP_LOGW` when active.

- [ ] **Step 3: Begin + image-descriptor guard**

Call `esp_https_ota_begin`. Then, under `#if CONFIG_OTA_VERSION_CHECK`, call `esp_https_ota_get_img_desc(handle, &img_desc)` and compare `img_desc.version` with `ota_manager_current_version()` via `ota_version_is_newer`. If the embedded version is **not** strictly newer (it disagrees with the `.version` file the core already accepted): `esp_https_ota_abort`, fire `OTA_EVENT_FAILED` with `ESP_ERR_INVALID_VERSION`, return that error. On match, fire no event — the core fired `OTA_EVENT_UPDATE_AVAILABLE` before calling the transport.

- [ ] **Step 4: Download loop with throttled progress**

Fire `OTA_EVENT_DOWNLOAD_START`. Loop `esp_https_ota_perform` while it returns `ESP_ERR_HTTPS_OTA_IN_PROGRESS`, computing `pct` as specified and firing `OTA_EVENT_PROGRESS` only when `pct` crosses a 10% boundary from the last reported value.

- [ ] **Step 5: Finalise + events**

On clean loop exit, fire `OTA_EVENT_DOWNLOAD_DONE`, then `OTA_EVENT_APPLYING`, then `esp_https_ota_finish`. If finish succeeds, fire `OTA_EVENT_SUCCESS`, return `ESP_OK`. On any error, fire `OTA_EVENT_FAILED` with `data.err`, call `esp_https_ota_abort`, return the error.

- [ ] **Step 6: Verify build with `CONFIG_OTA_TRANSPORT_HTTPS=y`**

Run `idf.py build`. Expected: no errors.

- [ ] **Step 7: Commit**

```bash
git add ota_manager/src/ota_transport_https.c
git commit -m "feat: HTTPS transport with img-desc guard and progress"
```

---

### Task 5: HTTP transport

**Files:**
- Modify: `src/ota_transport_http.c`

- [ ] **Step 1: Define transport perform function**

```c
esp_err_t ota_transport_http_perform(const char *url, ota_event_cb_t cb, void *cb_ctx);
```

- [ ] **Step 2: Init client, open, fetch headers**

`esp_http_client_init` with `url`, `timeout_ms = 30000`. Then `esp_http_client_open(client, 0)` and `int64_t content_len = esp_http_client_fetch_headers(client);`. If the HTTP status is not 200, fire `OTA_EVENT_FAILED`, clean up, return `ESP_FAIL`.

- [ ] **Step 3: Begin OTA**

`esp_ota_begin` on `esp_ota_get_next_update_partition(NULL)` with `OTA_WITH_SEQUENTIAL_WRITES` (`OTA_SIZE_UNKNOWN` when `content_len <= 0`). Fire `OTA_EVENT_DOWNLOAD_START`. (Do **not** fire `OTA_EVENT_UPDATE_AVAILABLE` here — the core fires it before calling the transport.)

- [ ] **Step 4: Download and write loop**

Heap-allocate a 4096-byte buffer with `malloc` (do **not** place it on the task stack — it would consume half of the default `OTA_TASK_STACK_SIZE` before HTTP/TLS frames and logging). If `malloc` returns NULL, fire `OTA_EVENT_FAILED` with `ESP_ERR_NO_MEM`, abort, and return. Loop `esp_http_client_read` into the buffer and `esp_ota_write` each chunk. Track bytes written; report progress at 10% increments using `content_len` as the denominator, or `0` throughout if `content_len <= 0`. `free` the buffer on every exit path (success and error).

- [ ] **Step 5: Finalise**

`esp_ota_end`, then `esp_ota_set_boot_partition`. Fire `OTA_EVENT_DOWNLOAD_DONE` → `OTA_EVENT_APPLYING` → `OTA_EVENT_SUCCESS` in sequence. On any error, fire `OTA_EVENT_FAILED`, call `esp_ota_abort`, clean up the client, return the error.

- [ ] **Step 6: Verify build with `CONFIG_OTA_TRANSPORT_HTTPS=n`**

Run `idf.py build`. Expected: no errors.

- [ ] **Step 7: Commit**

```bash
git add ota_manager/src/ota_transport_http.c
git commit -m "feat: HTTP transport fallback with open+fetch_headers"
```

---

### Task 6: Version fetch and wiring

**Files:**
- Modify: `src/ota_version.c`
- Modify: `src/ota_manager.c`

- [ ] **Step 1: Implement `ota_version_fetch`**

```c
esp_err_t ota_version_fetch(const char *base_url, char *out_buf, size_t buf_len);
```

Build the version URL by appending `.version` to `base_url` with `snprintf`; if the result would not fit `buf_len`, return `ESP_ERR_INVALID_SIZE`. GET via `esp_http_client` (open + fetch_headers) with `timeout_ms = 5000`. On HTTP 200, read the body into `out_buf` in a loop up to `buf_len - 1`, null-terminate, return `ESP_OK`. On any non-200 or transport error, return `ESP_FAIL`. (Guard IDF includes behind `#ifndef UNIT_TEST` as in Task 2.)

- [ ] **Step 2: Wire pre-download version gate into `ota_manager.c`**

Under `#if CONFIG_OTA_VERSION_CHECK`, implement a helper `static esp_err_t ota_version_gate(const char *url)`. Call `ota_version_fetch` into a 32-byte stack buffer. If it returns an error, fire `OTA_EVENT_FAILED` with that error and return it (do **not** treat as up-to-date). If it succeeds, call `ota_version_is_newer(remote, ota_manager_current_version())`. If not newer — including an unparseable body, which `ota_version_is_newer` reports as `false` — fire `OTA_EVENT_UP_TO_DATE` and return `ESP_ERR_NOT_FOUND`. If newer, return `ESP_OK`. Do **not** fire `OTA_EVENT_UPDATE_AVAILABLE` here; the caller (`ota_manager_check_and_update`) fires it once for both the check-on and check-off paths.

- [ ] **Step 3: Verify build with `CONFIG_OTA_VERSION_CHECK=y`**

Run `idf.py build`. Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add ota_manager/src/ota_version.c ota_manager/src/ota_manager.c
git commit -m "feat: .version fetch and pre-download version gate"
```

---

### Task 7: Check/update flow, poll task, manual trigger, transport dispatch

**Files:**
- Modify: `src/ota_manager.c`

- [ ] **Step 1: Implement `ota_manager_check_and_update`**

Take mutex. If `s_state != OTA_MGR_STATE_IDLE`, release and return `ESP_ERR_INVALID_STATE`. Set `s_state = OTA_MGR_STATE_CHECKING`, release. Resolve the URL (`s_cfg.server_url`); if empty, reset state to `IDLE` and return `ESP_ERR_INVALID_ARG`. Fire `OTA_EVENT_CHECK_START`.

```c
#if CONFIG_OTA_VERSION_CHECK
    esp_err_t g = ota_version_gate(url);   // fires UP_TO_DATE or FAILED itself
    if (g != ESP_OK) { reset_state_idle(); return g; }
#endif
    ota_fire_evt(OTA_EVENT_UPDATE_AVAILABLE);   // exactly once, both check-on and check-off
    esp_err_t result = s_transport_ops.perform(url, ota_internal_event, NULL);
    reset_state_idle();   // take mutex, s_state = IDLE, give (trampoline may have set DOWNLOADING/APPLYING)
```

Then handle reboot:
```c
    if (result == ESP_OK) {            // success path
#if CONFIG_OTA_AUTO_REBOOT
        vTaskDelay(pdMS_TO_TICKS(1000)); // let logs/events flush
        esp_restart();                   // does not return
#endif
    }
    return result;
```
`reset_state_idle()` is a one-line helper (mutex, write `IDLE`, give). There is no `FAILED` state to set — failure is carried by `result` and the `OTA_EVENT_FAILED` already fired by the transport or gate.

- [ ] **Step 2: Implement `ota_internal_event` trampoline**

Exactly as specified in the State Machine section: map `DOWNLOAD_START`→`DOWNLOADING` and `APPLYING`→`APPLYING`, then deliver every event via `ota_fire(ev)`. No `FAILED` case (the core resets state to `IDLE`).

- [ ] **Step 3: Implement the poll task**

```c
#if CONFIG_OTA_TRIGGER_POLL
static void ota_poll_task(void *arg) {
    // initial randomized delay to de-sync a fleet
    uint32_t base = s_cfg.poll_interval_sec;
    uint32_t first = esp_random() % (base < 300 ? base : 300);
    vTaskDelay(pdMS_TO_TICKS(first * 1000));
    while (1) {
        esp_err_t r = ota_manager_check_and_update();
        if (r == ESP_OK) {            // reached only if OTA_AUTO_REBOOT=n
            esp_restart();
        }
        uint32_t jitter = (base * CONFIG_OTA_POLL_JITTER_PCT / 100);
        uint32_t wait = base + (jitter ? (esp_random() % jitter) : 0);
        vTaskDelay(pdMS_TO_TICKS(wait * 1000));
    }
}
#endif
```

- [ ] **Step 4: Implement `ota_manager_start_task`**

```c
esp_err_t ota_manager_start_task(void) {
#if CONFIG_OTA_TRIGGER_POLL
    BaseType_t ok = xTaskCreate(ota_poll_task, "ota_mgr",
        CONFIG_OTA_TASK_STACK_SIZE, NULL, CONFIG_OTA_TASK_PRIORITY, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
#else
    ESP_LOGD(TAG, "manual trigger mode: start_task is a no-op");
    return ESP_OK;
#endif
}
```

- [ ] **Step 5: Select transport ops at init**

In `ota_manager_init`:
```c
#if CONFIG_OTA_TRANSPORT_HTTPS
    s_transport_ops.perform = ota_transport_https_perform;
#else
    s_transport_ops.perform = ota_transport_http_perform;
#endif
```

- [ ] **Step 6: Full build verify**

Run `idf.py build` with default Kconfig (HTTPS=y, POLL=y, VERSION_CHECK=y, ROLLBACK=y, AUTO_REBOOT=y). Then a second build with `OTA_TRIGGER_MANUAL=y` and `OTA_TRANSPORT_HTTPS=n`. Expected: both clean.

- [ ] **Step 7: Commit**

```bash
git add ota_manager/src/ota_manager.c
git commit -m "feat: check/update flow, poll task with jitter, manual trigger, dispatch"
```

---

### Task 8: README

**Files:**
- Create: `D:/projects/ESP32-components/ota_manager/README.md`

- [ ] **Step 1: Write README**

Sections required (prose, no filler):
- **Prerequisites:** `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` for secure HTTPS; rollback's `BOOTLOADER_APP_ROLLBACK_ENABLE` is selected automatically; two-OTA partition table.
- **Integration:** `idf_component.yml` snippet, with a note to prefer a relative/git path for shared repos.
- **Partition table:** the exact CSV from this spec.
- **Kconfig reference:** table of all `CONFIG_OTA_` options, types, defaults, descriptions.
- **Application usage:** the minimal `app_main` snippet from this spec, with the `confirm_valid()`-after-healthy emphasis.
- **Rollback contract:** one paragraph — the app must call `ota_manager_confirm_valid()` once healthy on every boot after enabling rollback; what `OTA_HEALTHCHECK_TIMEOUT_SEC` does; and that without it, reversion only happens on the next boot after a crash.
- **Reboot behaviour:** one paragraph on `OTA_AUTO_REBOOT` and why it must stay `y` in poll mode.
- **Version endpoint:** one paragraph on the `.version` URL convention, expected plain-text `X.Y.Z` response, and the `PROJECT_VER` requirement.
- **Security note (prominent):** `OTA_HTTPS_INSECURE` and the plain-HTTP transport disable transport authentication and there is no image signing in v1 — both are development-only; production should use HTTPS with a verified certificate.

- [ ] **Step 2: Commit**

```bash
git add ota_manager/README.md
git commit -m "docs: ota_manager v0.2.1 README"
```

---

### Task 9: End-to-end smoke test

**Files:**
- Modify: test project `main/app_main.c`

Performed in a real ESP32 test project, not inside the component directory.

- [ ] **Step 1: Set up test server and plan the version sequence**

The test drives three app versions: the device starts at `0.1.0`, a healthy `0.2.0` update, and a deliberately-bad `0.3.0` update used to force a rollback. Build each as a separate `firmware.bin` and serve it with a matching `firmware.bin.version` file. Start the server in the serving directory:
```bash
# firmware.bin.version contains a bare semver, e.g. "0.2.0"
python -m http.server 8080
```

- [ ] **Step 2: Configure and flash the baseline (0.1.0)**

In the test project set `PROJECT_VER` to `0.1.0`. Set `CONFIG_OTA_SERVER_URL="http://<host-ip>:8080/firmware.bin"`, `CONFIG_OTA_TRANSPORT_HTTPS=n`, `CONFIG_OTA_VERSION_CHECK=y`, `CONFIG_OTA_ROLLBACK_ENABLED=y`, `CONFIG_OTA_HEALTHCHECK_TIMEOUT_SEC=30`, `CONFIG_OTA_TRIGGER_MANUAL=y` (to control timing). The `app_main` calls `ota_manager_confirm_valid()` after Wi-Fi connects. Flash this 0.1.0 build.

- [ ] **Step 3: Verify baseline boot**

Confirm serial shows init success and partition validation pass. No `PENDING_VERIFY` warning yet — a freshly flashed image is already valid; that state only appears after an OTA.

- [ ] **Step 4: Healthy update 0.1.0 → 0.2.0**

Serve the `0.2.0` build (`firmware.bin` + `firmware.bin.version` = `0.2.0`). Trigger `ota_manager_check_and_update()` from a button/timer. Confirm the sequence: `CHECK_START` → `UPDATE_AVAILABLE` → `DOWNLOAD_START` → progress at 10% increments → `DOWNLOAD_DONE` → `APPLYING` → `SUCCESS` → reboot. After reboot confirm a `PENDING_VERIFY` warning (now running 0.2.0).

- [ ] **Step 5: Confirm healthcheck cancels rollback**

Confirm the 0.2.0 app calls `ota_manager_confirm_valid()` within 30s and no rollback occurs. Confirm absence of `PENDING_VERIFY` on the next manual reboot. Verify `ota_manager_current_version()` reports `0.2.0`.

- [ ] **Step 6: Forced rollback 0.2.0 → 0.3.0 (bad) → 0.2.0**

Build a `0.3.0` image with the `ota_manager_confirm_valid()` call commented out (a deliberately unhealthy build) and serve it (`firmware.bin.version` = `0.3.0`). From the running 0.2.0, trigger an update: `0.2.0 < 0.3.0` so it applies and reboots into 0.3.0 (`PENDING_VERIFY`). Because 0.3.0 never confirms, ~30s after boot the device logs the healthcheck error and reverts. Confirm it reboots and `ota_manager_current_version()` reports `0.2.0` again. Restore the `confirm_valid()` call in the source afterwards.

- [ ] **Step 7: Up-to-date path**

With 0.2.0 running, serve `firmware.bin.version` = `0.2.0` (same as running). Trigger again. Confirm `OTA_EVENT_UP_TO_DATE` fires and no download occurs.

- [ ] **Step 8: Final tag**

```bash
cd D:/projects/ESP32-components
git commit --allow-empty -m "chore: ota_manager v0.2.1 passes smoke test"
git tag ota_manager-v0.2.1
```

---

## Self-Review Checklist

- [x] All Kconfig symbols have corresponding implementation references in tasks
- [x] All public API functions (including `ota_manager_get_state`) have an implementing task
- [x] Trigger mode mutual exclusion expressed as a Kconfig `choice` (Task 1 Step 3)
- [x] `OTA_ROLLBACK_ENABLED` `select`s `BOOTLOADER_APP_ROLLBACK_ENABLE` (Task 1 Step 3)
- [x] Partition validation specified with exact `esp_partition_find_first` calls
- [x] Reboot governed by `OTA_AUTO_REBOOT`; poll task reboots on `ESP_OK`; `check_and_update` return contract matches the implementation
- [x] Healthcheck timer makes rollback time-bounded; `confirm_valid` cancels it; smoke test covers both confirm and timeout paths
- [x] Rollback timing described accurately (next-boot reversion, no running-image auto-revert without the timer)
- [x] Version comparator hardened (leading `v`, git-describe, numeric compare) and covered by a host unit test (Task 2)
- [x] Version-fetch failure fires `OTA_EVENT_FAILED`, not silent up-to-date (Task 6 Step 2)
- [x] HTTP transport opens connection and fetches headers before reading (Task 5 Steps 2–4)
- [x] `OTA_HTTPS_INSECURE` renamed; CN-only caveat and version-dependence documented
- [x] All declared events fire; `UPDATE_AVAILABLE` fires exactly once (in the core, never a transport); state machine wired via trampoline
- [x] No `assert` on resource allocation in library code; init and `start_task` return `esp_err_t`
- [x] Poll jitter + initial delay prevent fleet stampede; runtime interval clamped
- [x] HTTPS path validates embedded image descriptor; on mismatch it fails (`ESP_ERR_INVALID_VERSION`), it does not report up-to-date
- [x] No speculative features (MQTT trigger, NVS history, signed images excluded explicitly; signing flagged as the v2 candidate)

**v0.2.1 corrections (self-review of v0.2.0):**

- [x] Single `ota_fire`/`ota_fire_evt` delivery point; transports never call the app callback directly
- [x] `OTA_MGR_STATE_FAILED` removed; failure carried by `OTA_EVENT_FAILED` + return code; state resets to `IDLE` so the poll loop never wedges
- [x] `s_transport_ops` and `s_healthcheck_timer` explicitly declared; timer guarded by `ROLLBACK` (not the timeout) so `confirm_valid` always compiles
- [x] HTTP download buffer heap-allocated and freed on every path (off the task stack)
- [x] `esp_hw_support` in `REQUIRES` for `esp_random`; secure branch includes `esp_crt_bundle.h`
- [x] Fetch failure vs unparseable-but-fetched version are distinct and documented behaviours
- [x] Healthcheck timer uses task dispatch (never ISR); confirm/timer race acknowledged as a backstop, not a deadline
- [x] Smoke test manages the 0.1.0 → 0.2.0 → 0.3.0(bad) → 0.2.0 version sequence so the rollback path is actually exercised

# ota_manager — End-to-End Smoke Test (Task 9)

This is the **on-hardware** acceptance test for the `ota_manager` component. It
cannot be run without a physical ESP32 and a local HTTP server, so it is
provided as a ready-to-run kit rather than executed automatically.

It exercises the full lifecycle over the **plain-HTTP** transport (simplest to
serve locally) in **manual** trigger mode with **rollback + a 30 s healthcheck**:

```
0.1.0 (baseline)  →  0.2.0 (healthy update)  →  0.3.0 (bad → auto-rollback)  →  back to 0.2.0
```

> The component's secure default is HTTPS; this smoke test uses HTTP only so you
> can serve firmware from a laptop with `python -m http.server` and no certs.
> Do **not** ship `OTA_TRANSPORT_HTTPS=n`.

---

## 0. One-time test-app setup

Use any ESP-IDF app that depends on the component. The simplest is to copy
[`app_main.c`](app_main.c) from this directory into a fresh project's `main/`,
add the dependency, and use the partition table from the component README.

`main/idf_component.yml`:
```yaml
dependencies:
  ota_manager:
    path: "../../ota_manager"   # adjust to your layout
```

`sdkconfig.defaults` (the smoke-test config):
```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y

# OTA manager — smoke-test settings
CONFIG_OTA_TRANSPORT_HTTPS=n
CONFIG_OTA_VERSION_CHECK=y
CONFIG_OTA_ROLLBACK_ENABLED=y
CONFIG_OTA_HEALTHCHECK_TIMEOUT_SEC=30
CONFIG_OTA_TRIGGER_MANUAL=y
CONFIG_OTA_SERVER_URL="http://<HOST_IP>:8080/firmware.bin"
```

Set `<HOST_IP>` to your computer's LAN IP (not `127.0.0.1` — the ESP32 must
reach it). Fill in your Wi-Fi SSID/password at the top of `app_main.c`.

Use the partition CSV from `ota_manager/README.md` as `partitions.csv`.

---

## 1. Build the three firmware images and the version files

Each image is the **same app** built with a different `PROJECT_VER`. Set the
version per build with an environment variable consumed by the build (or a
`version.txt` in the project root containing just the version).

For each version, build and stash the resulting `build/<app>.bin`, and create a
matching `.version` file whose body is the bare semver:

```bash
# 0.1.0 — baseline (flashed directly, not served first)
idf.py -DPROJECT_VER=0.1.0 build
cp build/<app>.bin  serve/firmware_0.1.0.bin

# 0.2.0 — healthy update (app_main DOES call confirm_valid)
idf.py -DPROJECT_VER=0.2.0 build
cp build/<app>.bin  serve/firmware_0.2.0.bin
printf '0.2.0' > serve/firmware_0.2.0.bin.version

# 0.3.0 — deliberately BAD: comment out the ota_manager_confirm_valid() call,
#          rebuild, then restore it afterwards
idf.py -DPROJECT_VER=0.3.0 build
cp build/<app>.bin  serve/firmware_0.3.0.bin
printf '0.3.0' > serve/firmware_0.3.0.bin.version
```

To "serve version N", copy that image to the served name the device requests:
```bash
cp serve/firmware_<V>.bin          serve/firmware.bin
cp serve/firmware_<V>.bin.version  serve/firmware.bin.version
```

Start the server from the `serve/` directory:
```bash
cd serve && python -m http.server 8080
```

---

## 2. Flash the baseline (0.1.0) and verify boot

Flash the 0.1.0 build to the device:
```bash
idf.py -DPROJECT_VER=0.1.0 -p <PORT> flash monitor
```
**Expect:** init succeeds, partition validation passes, Wi-Fi connects,
`ota_manager_current_version()` logs `0.1.0`. **No** `PENDING_VERIFY` warning —
a freshly flashed image is already valid; that state only appears after an OTA.

## 3. Healthy update 0.1.0 → 0.2.0

Serve the 0.2.0 image (`firmware.bin` + `firmware.bin.version` = `0.2.0`).
The app triggers `ota_manager_check_and_update()` on its timer.

**Expect this event sequence** (from the callback log):
```
CHECK_START → UPDATE_AVAILABLE → DOWNLOAD_START → PROGRESS (0,10,…,100) →
DOWNLOAD_DONE → APPLYING → SUCCESS → (reboot)
```
After reboot, **expect** a `PENDING_VERIFY` warning (now running 0.2.0,
not yet confirmed).

## 4. Healthcheck confirms (no rollback)

The 0.2.0 `app_main` calls `ota_manager_confirm_valid()` once Wi-Fi is up,
within the 30 s window. **Expect:** no rollback; a manual reset boots 0.2.0
cleanly with **no** `PENDING_VERIFY` warning. `current_version()` = `0.2.0`.

## 5. Up-to-date path

Keep serving `firmware.bin.version` = `0.2.0` (same as running). On the next
check, **expect** `OTA_EVENT_UP_TO_DATE` and **no** download.

## 6. Forced rollback 0.2.0 → 0.3.0(bad) → 0.2.0

Serve the **bad** 0.3.0 image (built with `confirm_valid()` commented out) and
`firmware.bin.version` = `0.3.0`. Since `0.2.0 < 0.3.0`, it applies and reboots
into 0.3.0 (`PENDING_VERIFY`). Because 0.3.0 never confirms, ~30 s after boot:

**Expect:** the healthcheck `ESP_LOGE` fires, the device invalidates the image
and reboots, and `current_version()` reports `0.2.0` again. Restore the
`confirm_valid()` call in the source afterwards.

## 7. Result

The test passes if: the healthy update applied and confirmed (steps 3–4), the
up-to-date path skipped the download (step 5), and the unhealthy image
auto-reverted within the healthcheck window (step 6).

---

## Notes / gotchas

- `PROJECT_VER` must be a clean `X.Y.Z` (optionally `v`-prefixed). A non-numeric
  version is rejected by the comparator and **no** update is ever applied.
- `CONFIG_OTA_HEALTHCHECK_TIMEOUT_SEC=30` is generous enough for a healthy boot;
  the timer is a backstop, not a tight deadline (see the component README's
  rollback contract and the healthcheck-race note).
- In manual mode `CONFIG_OTA_AUTO_REBOOT` may be `y` or `n`; this app assumes the
  default `y` (the device reboots inside `check_and_update` on success). If you
  set it `n`, the app must reboot itself after `OTA_EVENT_SUCCESS`.

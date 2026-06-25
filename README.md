# ESP32-components

A collection of reusable, drop-in **ESP-IDF 5.x** components for the ESP32 family
(ESP32 / ESP32-S3 / ESP32-C3). Each component lives in its own top-level
directory and is consumed by other projects via a path or git reference in
`idf_component.yml` — there is no monolithic build here.

Developed and verified against **ESP-IDF v5.5.4** (5.1 minimum).

---

## Components

| Component | Version | Description | Docs |
|---|---|---|---|
| [`ota_manager`](ota_manager/) | `0.2.1` | Configurable OTA firmware-update manager: HTTPS (cert-bundle) or HTTP transport, tolerant semver version gating, A/B rollback with an optional boot-time healthcheck timer, and poll **or** manual trigger modes — all selected at compile time via Kconfig. | [ota_manager/README.md](ota_manager/README.md) |

`ota_manager` is **hardware-validated** end-to-end on an ESP32 (healthy update,
up-to-date, and forced-rollback paths) — see the kit in [`smoke_test/`](smoke_test/).

---

## Repository layout

```
ESP32-components/
  ota_manager/                  # the OTA component (CMake + Kconfig + src + include + test)
  smoke_test/                   # on-hardware acceptance-test kit for ota_manager
  ota_manager_spec_v0.2.1.md    # design spec / implementation plan for ota_manager
  README.md                     # this file
```

---

## Using a component

Add it to your project's `main/idf_component.yml`. Prefer a **git reference**
(portable across machines and CI) and pin to a tag:

```yaml
dependencies:
  ota_manager:
    git: "https://github.com/rippoks/ESP32-components.git"
    path: "ota_manager"
    version: "ota_manager-v0.2.1"
```

Or, for local development on a checkout of this repo, a **path reference**:

```yaml
dependencies:
  ota_manager:
    path: "../../ESP32-components/ota_manager"   # adjust to your layout
```

Each component documents its own prerequisites (partition table, sdkconfig,
required call order, etc.) in its README — read it before integrating.

---

## Versioning

Components are versioned independently. A released component state is marked
with a git tag of the form `<component>-v<semver>` (e.g. `ota_manager-v0.2.1`),
and the same version is set in the component's `idf_component.yml`.

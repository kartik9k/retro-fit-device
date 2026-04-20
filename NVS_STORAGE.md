# NVS Storage Design

> **Owner: Firmware Agent.**
> This file is the single source of truth for all Non-Volatile Storage (NVS)
> layout on the device.
>
> **Change protocol:**
> - Any code change that adds, removes, or modifies an NVS namespace or key
>   **must** update this document in the same commit.
> - If a key is renamed or its type changes, note the migration strategy —
>   old keys are not automatically erased on firmware update; stale data can
>   cause unexpected behaviour on first boot after an OTA or reflash.
> - The NVS partition is erased automatically if `nvs_flash_init()` returns
>   `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND` (see
>   `wifi_manager.c:wifi_manager_init`). This erases **all** namespaces —
>   document any key that must survive an erase (it cannot; design accordingly).

---

## Partition

| Field | Value |
|-------|-------|
| Partition label | `nvs` (default) |
| Partition size | Defined in partition table (`partitions.csv` or SDK default) |
| Initialisation | `nvs_flash_init()` in `wifi_manager_init()` — called once at boot before any NVS read |
| Erase on error | Yes — partition is erased and reinitialised if layout has changed |

> **Note (DEFERRED §4):** When NVS-based runtime transport selection is
> implemented, `nvs_flash_init()` must be moved from `wifi_manager_init()` to
> `app_main()` so `transport_get()` can read its key before `wifi_transport.init()`
> is called. Update this document at that time.

---

## Namespaces and Keys

### `wifi_mgr`

Managed by: `main/wifi_manager.c`

| Key | NVS type | Max value size | Default (absent) | Description |
|-----|----------|----------------|------------------|-------------|
| `ssid` | `NVS_TYPE_STR` | 33 bytes (32 chars + null) | *(absent)* | Wi-Fi network SSID. Absent on a virgin device; triggers provisioning mode when missing. |
| `pass` | `NVS_TYPE_STR` | 65 bytes (64 chars + null) | `""` (empty string) | Wi-Fi password. May be empty for open networks. |

**Write path:** Captive-portal HTTP POST handler (`handle_post`) — written once
during provisioning, then read on every subsequent boot.

**Read path:** `nvs_load()` in `wifi_manager_init()` — if both keys are present
and `ssid` is non-empty, the device connects directly. If absent or empty,
provisioning mode starts.

**Erase behaviour:** Erasing NVS removes these keys; the device re-enters
provisioning mode on next boot. This is the intended factory-reset mechanism.

---

### `transport` *(planned — not yet implemented)*

> See `DEFERRED.md` §4. This namespace does not exist yet. Documented here
> so the design is visible before implementation begins.

Managed by: `main/transport.c` *(future)*

| Key | NVS type | Max value size | Default (absent) | Description |
|-----|----------|----------------|------------------|-------------|
| `type` | `NVS_TYPE_STR` | 10 bytes | `"wifi"` | Active transport: `"wifi"` or `"cellular"`. Read once in `transport_get()` at boot. |

**Write path:** Not written by firmware — set externally (OTA config, factory
flash tool, or a future configuration endpoint).

**Read path:** `transport_get()` in `main/transport.c` — once at boot, before
`transport->init()` is called.

---

## Key size constants (firmware reference)

Defined in `main/wifi_manager.c`:

| Constant | Value | Purpose |
|----------|-------|---------|
| `WM_SSID_LEN` | 33 | Buffer size for SSID read from NVS |
| `WM_PASS_LEN` | 65 | Buffer size for password read from NVS |

---

## Change Log

| Date | Change | Raised by |
|------|--------|-----------|
| 2026-04-20 | Initial NVS storage design documented | Firmware Agent |

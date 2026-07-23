# bb_storage_nvs

ESP-IDF NVS backend for `bb_storage`.

## Two access paths

This component serves two independent callers:

1. **`bb_nv`'s typed forwarders.** `bb_nv_get_u8/set_u8/get_u16/.../get_str/
   set_str/erase/erase_namespace/exists` (`platform/espidf/bb_nv/bb_nv.c`)
   are thin forwarders to this component's typed accessors
   (`bb_storage_nvs_get_u8`, `bb_storage_nvs_set_str`, ...) — the exact
   `nvs_open`/`nvs_get_u8`/`nvs_set_str`/type-mismatch-handling logic that
   used to live directly in `bb_nv.c`, moved here verbatim. **On-flash
   format is unchanged** — no migration, same NVS entry types
   (U8/U16/U32/STR) at the same namespace/key strings.
2. **The generic `bb_storage` facade.** `bb_storage_nvs_register()` wires
   this component's `bb_storage_vtable_t` implementation into `bb_storage`
   under the backend name `"nvs"` — `bb_storage_addr_t{ns_or_dir, key}`
   maps to `{NVS namespace, NVS key}`, and values are stored/read as raw
   blobs (`nvs_set_blob`/`nvs_get_blob`). This path is independent of (1)
   and is not used by any `bb_nv_*` call.

These two paths are deliberately separate: NVS's typed entries (U8/U16/U32/
STR) are not representable as a single blob shape without migrating every
existing on-flash `bb_nv` entry, so the typed accessors and the generic
blob vtable coexist rather than one being built on top of the other.

## Registration

Composition-only, mirroring `bb_storage`'s rule: call
`bb_storage_nvs_register()` explicitly to wire the generic blob-vtable path
in under the name `"nvs"`. No self-registration, no `AUTOREGISTER` Kconfig.
The typed-accessor path (used by `bb_nv`) requires no registration — it is
called directly.

## bb_nv_batch_* is out of scope

`bb_nv_batch_begin/set_u*/set_str/commit` stay implemented in
`platform/espidf/bb_nv/bb_nv.c`, keyed directly on an `nvs_handle_t` — the
batch API is deliberately NVS-backend-private and not part of the generic
`bb_storage_vtable_t` (which has no multi-key-transaction concept).

## Truncated reads via the generic facade

`bb_storage_get()` with `cap` smaller than a stored blob's length bounces
through a bounded on-stack scratch buffer (`BB_STORAGE_NVS_GET_SCRATCH_MAX`,
default 512 bytes, no heap). A blob larger than that limit that also needs
truncation returns `BB_ERR_NO_SPACE`. This limit does not apply to the typed
accessors (path 1 above), which read directly into the caller's buffer.

## API

See `include/bb_storage_nvs.h` and `bb_storage.h` (the facade the generic
vtable plugs into).

#pragma once
// B1-1274: consumer-owned `bbtool codegen --consumer-manifest` file (see
// scripts/bbtool/README.md's "`args=` (parameterized init calls) and
// `--consumer-manifest`" section) -- declares init markers that must NOT
// live inside a component header, because bb_wifi_prov must never
// self-register its own default provisioning form (B1-966, enforced by the
// `prov-default-form-internal-ref` lint). This file is the composition
// root's opt-in, exactly what the lint expects to be the only caller of
// bb_wifi_prov_default_form_get() outside bb_wifi_prov's own default_form/
// TU.
//
// tier=regular (not early/pre_http): bb_wifi_prov_autoinit() calls
// bb_wifi_prov_start(), which internally calls bb_http_server_ensure_started()
// itself (platform/espidf/bb_wifi_prov/bb_wifi_prov.c), so it does not
// strictly need the pre_http-tier http_server-providing entry to have run
// first. What DOES matter is registration order relative to every other
// composed component's own HTTP routes: bb_wifi_prov_start() registers a
// captive-portal `GET /*` wildcard, and esp_http_server's wildcard matching
// is first-match-wins in registration order -- a wildcard registered before
// another component's specific route would shadow it for the entire
// provisioning window (see bb_wifi_prov.h's registration-order invariant,
// and examples/floor/main/floor_app.c's own bb_wifi_prov_autoinit() call
// site, which sequences it after floor's HTTP lifecycle service has
// registered its own routes for the identical reason).
//
// B1-1280: this marker carries `provides=http_wildcard_last` -- the
// reserved key `bbtool codegen` (scripts/bbtool/commands/wire.py's
// render_source) machine-enforces: it hard-errors at codegen time if any
// `server=true` entry (every other composed component's route registration
// -- bb_wifi_http, bb_ota_pull, bb_diag_http, bb_sensor_http, etc.) sorts at
// or after this entry in the final composed order, naming both entries'
// file:line. Getting this ordering right is no longer this comment's job --
// it is `render_source`'s -- so the comment above only needs to explain WHY
// the order matters, not carry the invariant itself; if this marker is ever
// composed after some new consumer route with no explicit order=, codegen
// fails loudly instead of silently shadowing that route.
//
// WARNING: if a second manifest entry is added to this file, manifest entries
// tie-break by parse order (line order), so the captive-portal entry must
// remain textually last, OR both entries must be disambiguated with explicit
// order= values -- the guard above catches a `server=true` route landing
// after this one, but says nothing about ordering between two non-server=
// manifest entries.
//
// B1-1037 PR-1: the "wifi" bb_lifecycle service registration below is a
// SECOND manifest entry, added here (rather than a new manifest file)
// because `bbtool codegen --consumer-manifest` accepts exactly one path
// (scripts/bbtool/README.md), so smoke has only one manifest home. It needs
// no order=/textual-position care relative to the captive-portal entry
// above: tier is the primary sort key (wire_graph.topo_sort), and
// tier=early always sorts strictly before this file's tier=regular entry
// regardless of parse order -- only two entries in the SAME tier need the
// disambiguation the WARNING above describes. This registration is
// deliberately INERT: registered-but-unstarted (bb_lifecycle_register()
// leaves the service in BB_LIFECYCLE_STOPPED) is a harmless no-op. It does
// NOT restore bb_mdns/bb_mqtt_client's "wifi" service auto-start on its own
// -- the service only reaches RUNNING once bb_wifi_set_emit() is wired to
// drive it, which is PR-4 (out of scope here; see bb_lifecycle.h's
// bb_lifecycle_emit_binding_init()/bb_wifi_set_emit doc).
#include "bb_lifecycle.h"
#include "bb_wifi_prov.h"
#include "bb_wifi_prov_default_form.h"
#include "smoke_routes.h"
#include "bb_system_routes.h"
#include "bb_health.h"
#include "bb_openapi.h"
#include "bb_storage_http.h"
#include "bb_log_http.h"
#include "bb_diag_http.h"
#include "bb_wifi_http.h"
#include "bb_sensor_http.h"
#include "bb_mdns.h"
#include "bb_log_event.h"
#include "bb_log_event_wire.h"
#include "bb_data.h"
#include "bb_data_http.h"
#include "smoke_core_claim.h"
#include "smoke_data_notify.h"

// bbtool:init tier=early fn=bb_lifecycle_register out=s_smoke_wifi_svc:bb_lifecycle_svc_t args=&(bb_lifecycle_config_t){.name="wifi"},&s_smoke_wifi_svc

// B1-1364 PR5 validation: smoke_core_claim_start/_log_httpd_core
// (examples/smoke/main/smoke_core_claim.c) are app-level code, not a
// component intrinsic (mirrors the app-route markers below), so their
// markers live here rather than in any component header. Both entries are
// no-op BB_OK stubs unless CONFIG_BB_SMOKE_CORE_CLAIM is on (see
// smoke_core_claim.c) -- every existing smoke env is unaffected.
//
// smoke_core_claim_start carries `order=0`: no OTHER pre_http-tier entry in
// this tree sets an explicit `order=` (verified against every `// bbtool:
// init tier=pre_http` marker in the repo), so `order=0` deterministically
// sorts this entry FIRST among pre_http-tier entries with no requires/
// provides edge -- specifically, before bb_http_autostart_init()
// (components/bb_http_server/include/bb_http_server.h, provides=
// http_server), which is the ordering the proof depends on: the core must
// be claimed via bb_wdt_claim_core_exclusive() (inside bb_task_create())
// BEFORE httpd_start() reads bb_wdt_claimed_core_mask() to steer its
// worker's core_id (see bb_wdt.h's "Ordering requirement" note).
//
// smoke_core_claim_log_httpd_core carries `tier=regular` instead (NOT
// `requires=http_server`) -- `requires=`/`provides=` is a topo_sort-only
// ordering edge (wire_graph.topo_sort), which governs the entry list
// `render_source` receives but is NOT what render_source itself consults
// when placing the http_server-providing entry: render_source special-cases
// that ONE entry out of the tier=pre_http loop entirely. It is excluded from
// the `pre_http_no_server` list, and that list -- every OTHER pre_http entry
// -- is rendered first, in full, still in its topo-sorted position; only
// AFTER that loop finishes does render_source emit the captured
// `__auto_type bb_app_http_handle = ...` line for the excluded entry, never
// before. It then unconditionally renders the ENTIRE `regular` tier after
// that capture line. A tier=pre_http entry -- even with
// `requires=http_server` -- still lands in the pre_http_no_server loop,
// which runs BEFORE the capture line, so the guard it depends on
// (`bb_app_avail_http_server`) is still false when it is called and the
// call is skipped at runtime (verified against the generated bb_app_init.c;
// this was the actual defect, tracked separately). tier=regular is the only
// marker shape that is reliably ordered after the capture line: the entire
// regular block is always emitted after that line regardless of any
// `order=` on a regular entry, since `order=` only reorders an entry
// against other regular entries and can never move it ahead of the capture
// -- a by-construction guarantee of render_source's fixed emission order,
// not an incidental property of this composition. It needs no requires=
// edge (or server=true, since this fn takes no handle argument) to get that
// guarantee.

// bbtool:init tier=pre_http order=0 fn=smoke_core_claim_start

// bbtool:init tier=regular fn=smoke_core_claim_log_httpd_core

// B1-1315: bb_system_routes_init/bb_health_init/bb_openapi_init are
// route-registering registry hooks relocated out of their component headers
// (B1-1279/B1-1314) -- a component composing bb_system for its
// reboot-reason SSOT (etc.) must not also be forced to expose POST
// /api/diag/reboot. `component=` (B1-1275) pulls in each component's own
// REQUIRES/PRIV_REQUIRES closure so the manifest entry composes correctly.
// See bb_wifi_prov_autoinit below for the precedent this mirrors.

// bbtool:init tier=regular fn=bb_system_routes_init server=true component=bb_system binds_data=reboot

// bbtool:init tier=regular fn=bb_health_init server=true component=bb_health

// bbtool:init tier=regular fn=bb_openapi_init server=true component=bb_openapi

// B1-1316: bb_storage_http_routes_init/bb_storage_http_factory_reset_routes_init/
// bb_log_register_routes_init/bb_diag_routes_init/bb_diag_sections_init
// (bb_diag_http component) plus bb_wifi_routes_init (bb_wifi_http) and
// bb_sensor_http_init (bb_sensor_http) are the second batch of
// route-registering registry hooks relocated out of their component headers
// (B1-1279/B1-1314) -- most pointedly bb_diag_routes_init/
// bb_diag_sections_init, which register GET /api/diag/meminfo among other
// diagnostics: a component composed only for its SSOT (reset-reason latch,
// panic capture, WiFi STA core, sensor bb_data keys) must not also be
// forced to expose its full HTTP surface to an unauthenticated provisioning
// SoftAP client (B1-1279, hardware-confirmed). `component=` (B1-1275) pulls
// each component's own REQUIRES/PRIV_REQUIRES closure so the manifest entry
// composes correctly.

// bbtool:init tier=regular fn=bb_storage_http_routes_init server=true component=bb_diag_http binds_data=factory_reset,storage_delete

// bbtool:init tier=regular fn=bb_storage_http_factory_reset_routes_init server=true component=bb_diag_http binds_data=factory_reset,storage_delete

// bbtool:init tier=regular fn=bb_log_register_routes_init server=true component=bb_diag_http

// bbtool:init tier=regular fn=bb_diag_routes_init server=true component=bb_diag_http

// bbtool:init tier=regular fn=bb_diag_sections_init server=true component=bb_diag_http

// bbtool:init tier=regular fn=bb_wifi_routes_init server=true component=bb_wifi_http binds_data=wifi

// bbtool:init tier=regular fn=bb_sensor_http_init server=true component=bb_sensor_http binds_data=fan,power,thermal

// B1-1357: the OTA family (bb_ota_boot/bb_ota_hooks/bb_ota_validator/
// bb_ota_check/bb_ota_pull/bb_ota_push) needs a northstar refactor before
// it is composed here again -- all six `// bbtool:init` markers are
// removed from this file, and examples/smoke/main/smoke_app.c carries no
// handwired call into any of them either. Boards in this interim get
// updated by serial flash, an accepted consequence rather than an
// oversight. The components themselves stay in the tree untouched.
//
// bb_ota_validator is the one exception to "nothing OTA links": it is
// still pulled in transitively via bb_diag_http's PRIV_REQUIRES
// (components/bb_diag_http/CMakeLists.txt) for its on_validated hook
// (platform/espidf/bb_diag_http/bb_diag_http_routes.c), just with its own
// routes no longer registered here. Tracked separately as B1-1367, not
// fixed by this file.

// B1-1318 (epic B1-1314), fourth batch: bb_mdns_registry_init and
// bb_log_event_init are route-registering registry hooks relocated out of
// their component headers (B1-1279/B1-1314), same shape as batches 1-3.
// `component=` (B1-1275) pulls each component's own REQUIRES/PRIV_REQUIRES
// closure so the manifest entry composes correctly. bb_log_event_init's
// source header is platform/espidf/bb_log_event/bb_log_event.h -- a
// flat (no include/ subdir) platform-only header, not a components/<name>/
// include/ one; wire.py's _component_headers falls back to the component's
// flat dir the same way for both components/ and platform/ layouts, so
// relocating a platform-only marker behaves identically to a components/
// one (verified against wire.py's docstring and this batch's generated
// bb_app_init.c diff).
//
// bb_display_info_register_init (components/display/bb_display/include/
// bb_display_info.h) is DELIBERATELY NOT relocated here, unlike the other
// two -- its header marker is deleted (closing the real B1-1279 exposure: a
// direct `bbtool codegen --board elecrow_p4_hmi7` omitting `--wire-board`
// would otherwise auto-scan bb_display's header via collect_entries and
// force-register the route) but no manifest entry replaces it. Smoke's wire
// generation is deliberately board-invariant (`--wire-board
// smoke_wire_baseline`, shared by all four `smoke-gen-*` Makefile targets),
// while `component=` folds into the REQUIRES/components-fragment resolution
// PER REAL --board (codegen.py's run(), before resolve_composition_with_
// graph). Adding `component=bb_display` here would therefore pull bb_display
// into REQUIRES for every smoke board (not just the two display boards that
// add it via bbtool.toml's per-board add_components) and emit the call
// unconditionally, since wire generation doesn't vary by board -- an
// unvalidated runtime behavior change on display boards, exactly what
// bbtool.toml's `[board.smoke_wire_baseline]` comment already flags as
// needing hardware validation via a separate ticket. Removing the marker
// changes nothing for any build today: wire generation's fixed baseline
// never included bb_display, so the function has zero generated call sites
// in this repo currently. bb_display_info_register_init becomes reachable
// only via explicit handwire (matching its current de-facto status);
// bb_display_register_info() -- the different function entry_espidf.c
// already handwires under `__has_include("bb_display_info.h")` -- is
// untouched.

// bbtool:init tier=regular fn=bb_mdns_registry_init server=true component=bb_mdns

// bbtool:init tier=regular fn=bb_log_event_init server=true component=bb_log_event

// B1-1425: link bb_data_http's ESP-IDF backend into smoke -- previously
// compiled (components/bb_data_http/CMakeLists.txt folds
// platform/espidf/bb_data_http/bb_data_http_espidf.c straight into the
// component's own SRCS, backend-dispatch style) but never called anywhere
// under examples/, so ESP-IDF's `-ffunction-sections`/`--gc-sections` build
// dropped every bb_data_http_espidf_* symbol from firmware.elf on all nine
// CI boards. bb_data_http itself is already reachable here (bb_diag_http's
// own PRIV_REQUIRES pulls it into BB_AUTOWIRE_REQUIRES's closure, which is
// how entry_espidf.c's existing bb_data_http_describe_foreach() call
// already links) -- but per the SAME B1-1315/B1-1316 rationale as the
// bb_diag_http/bb_wifi_http/bb_sensor_http route markers above, a component
// being composed for one reason (here, bb_data_http's describe-table seam)
// must not silently expose its full HTTP surface as a side effect;
// registering GET /api/events and GET /ws/events is this file's explicit
// opt-in, mirroring examples/floor/main/floor_app.c's http_lifecycle_
// observer (bb_data_http_espidf_start() then _routes_init()) as manifest
// entries instead of a lifecycle-observer callback, since smoke starts its
// HTTP server via the plain `provides=http_server` pre_http entry
// (bb_http_autostart_init, components/bb_http_server/include/
// bb_http_server.h) rather than floor's handwired lifecycle-observer start/
// stop pair.
//
// bb_data_http_espidf_ws_routes_init: smoke's esp32 sdkconfig carries
// CONFIG_HTTPD_WS_SUPPORT=y (examples/smoke/sdkconfig.esp32), unlike
// examples/floor which has it off -- so this is the one example where the
// WS egress route genuinely links (and can be exercised), not merely
// compiles.
//
// REAL prerequisite chain (review fix -- link-exercised alone left every
// endpoint dead on arrival, since bb_data_http_client_acquire()
// (components/bb_data_http/src/bb_data_http_common.c) gates on
// bb_data_http_init() having run and returns BB_ERR_INVALID_STATE
// otherwise, and neither espidf_start() nor either routes_init() calls it):
//
//   bb_data_http_init(NULL)  -- provides=data_http_init. cfg=NULL (Kconfig
//     defaults, CONFIG_BB_DATA_HTTP_MAX_CLIENTS/_EVENT_RING_CAPACITY) --
//     mirrors examples/floor/main/floor_app.c's own bb_data_http_init(NULL)
//     call; smoke has no board-specific reason to size differently from
//     what the same Kconfig already governs per-board.
//   bb_data_bind("log", ...)  -- provides=data_log_bound. Binds the "log"
//     dissolved-bb_event producer key into bb_data (same key/desc/gather
//     shape as floor_app.c's producers[] loop, smoke_log_fill_ctx defined
//     in smoke_app.c/declared in smoke_routes.h) -- independent of
//     bb_data_http entirely, so no requires= on data_http_init.
//   bb_data_http_espidf_start()  -- requires=data_http_init ONLY (review fix:
//     bb_data_http_client_acquire(), components/bb_data_http/src/
//     bb_data_http_common.c, gates solely on s_cfg.initialized, set by
//     bb_data_http_init() -- no bound key is a real prerequisite for
//     starting the broadcaster task. requires=data_log_bound here would be
//     a stylistic "bind-before-start" nicety at best today, but with a
//     second attached key it would be an actual bug: one failed
//     bb_data_bind() would wrongly skip starting the broadcaster for every
//     OTHER already-attached key too). provides=data_http_started.
//   bb_data_http_attach({.key="log", ...})  -- requires=data_http_init,
//     data_log_bound (attaches "log" into bb_data_http's OWN topic table
//     so /api/events?topic=log and /ws/events actually stream it -- without
//     this, both endpoints would link and accept connections but carry
//     zero traffic, the same defect in a new place). provides=
//     data_log_attached.
//   bb_data_http_espidf_routes_init(server) / _ws_routes_init(server)  --
//     requires=data_http_started,data_log_attached. Registering GET
//     /api/events / GET /ws/events before the broadcaster task exists
//     (data_http_started) or before any key is attached (data_log_attached)
//     would accept connections that stream nothing forever.

// bbtool:init tier=regular fn=bb_data_http_init component=bb_data_http args=NULL provides=data_http_init

// bbtool:init tier=regular fn=bb_data_bind component=bb_data provides=data_log_bound args=&(bb_data_binding_t){.key="log",.desc=&bb_log_event_wire_desc,.gather=bb_data_gather_plain,.ctx=(void*)&smoke_log_fill_ctx}

// bbtool:init tier=regular fn=bb_data_http_espidf_start component=bb_data_http requires=data_http_init provides=data_http_started

// bbtool:init tier=regular fn=bb_data_http_attach component=bb_data_http requires=data_http_init,data_log_bound provides=data_log_attached args=&(bb_data_http_attach_cfg_t){.key="log",.topic="log",.kind=BB_DATA_HTTP_STATE,.snap_size=bb_log_event_wire_desc.snap_size}

// bbtool:init tier=regular fn=bb_data_http_espidf_routes_init server=true component=bb_data_http requires=data_http_started,data_log_attached

// bbtool:init tier=regular fn=bb_data_http_espidf_ws_routes_init server=true component=bb_data_http requires=data_http_started,data_log_attached

// B1-1451 (epic B1-1123): "smoke_notify" is a SECOND bb_data_http-attached
// key, EVENT-kind (unlike "log" above, which is STATE-kind) -- the on-device
// proof harness for bb_data_http_notify_push() (examples/smoke/main/
// smoke_data_notify.c/.h; see that file's own header comment for the full
// rationale + off-board verification recipe).
//
// Unlike the "log" key above, bind/attach here go through smoke_data_notify_
// bind()/_attach() WRAPPER functions, not bb_data_bind()/bb_data_http_
// attach() called directly with `args=` -- `bbtool codegen` markers
// have no Kconfig gate of their own (they always fire), so the ONLY way to
// make this binding disappear entirely on a default smoke build is to gate
// the library calls THEMSELVES inside a wrapper (B1-1451 review fix: an
// earlier version bound/attached this key unconditionally, permanently
// consuming a bb_data binding-table slot on every board regardless of
// whether this demo is ever enabled). Both wrappers -- and
// smoke_data_notify_routes_init() below -- are no-op BB_OK stubs when
// CONFIG_BB_SMOKE_DATA_NOTIFY is off, mirroring smoke_core_claim's always-
// called/internally-gated shape.
//
//   smoke_data_notify_bind()  -- provides=data_notify_bound.
//   smoke_data_notify_attach()  -- requires=data_http_init,data_notify_bound.
//     provides=data_notify_attached.
//   smoke_data_notify_routes_init(server)  -- requires=data_http_started,
//     data_notify_attached (same ordering rationale as the "log" key's own
//     routes_init markers above: routes must not accept connections before
//     the broadcaster exists or the key is attached).

// bbtool:init tier=regular fn=smoke_data_notify_bind provides=data_notify_bound

// bbtool:init tier=regular fn=smoke_data_notify_attach requires=data_http_init,data_notify_bound provides=data_notify_attached

// bbtool:init tier=regular fn=smoke_data_notify_routes_init server=true requires=data_http_started,data_notify_attached

// B1-1274-adjacent: GET /ping, GET /ws, and POST /api/wsbcast are smoke's
// own app-level routes (examples/smoke/main/smoke_app.c), not a component
// intrinsic -- the URI and handler are consumer policy, so these markers
// live here rather than in any component header (mirrors the
// bb_wifi_prov_autoinit marker below, which the same rule places here for
// bb_wifi_prov_default_form_get()). Each is `args=` (not `server=true`)
// because the handle is threaded explicitly via `bb_app_http_handle` --
// per wire_parse's module docstring, `args=`/`server=` are mutually
// exclusive, so each of these three also carries `registers_routes=true`
// (B1-1280) to opt back into the automatic `http_wildcard_last` ordering
// guard that `server=true` entries get for free: codegen hard-errors if any
// of these three sorts at or after bb_wifi_prov's captive-portal marker
// below. They are also placed textually BEFORE that marker (manifest
// entries with no explicit `order=` tie-break by parse/line order --
// wire_graph.topo_sort) to keep them ahead of bb_wifi_prov's catch-all
// `GET /*`, but the guard -- not just this placement -- is what makes a
// future reorder fail loudly instead of silently shadowing a route. Do not
// reorder below the captive-portal marker.
//
// bbtool:init tier=regular fn=bb_http_register_route registers_routes=true args=bb_app_http_handle,BB_HTTP_GET,"/ping",smoke_ping_handler

// bbtool:init tier=regular fn=bb_ws_server_register_described_endpoint registers_routes=true args=bb_app_http_handle,"/ws",smoke_ws_echo_handler,&smoke_ws_route

// bbtool:init tier=regular fn=bb_http_register_route registers_routes=true args=bb_app_http_handle,BB_HTTP_POST,"/api/wsbcast",smoke_wsbcast_handler

// bbtool:init tier=regular fn=bb_wifi_prov_autoinit args=bb_wifi_prov_default_form_get(),1,NULL provides=http_wildcard_last

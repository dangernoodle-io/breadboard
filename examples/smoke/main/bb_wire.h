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

// bbtool:init tier=early fn=bb_lifecycle_register out=s_smoke_wifi_svc:bb_lifecycle_svc_t args=&(bb_lifecycle_config_t){.name="wifi"},&s_smoke_wifi_svc

// B1-1315: bb_system_routes_init/bb_health_init/bb_openapi_init are
// route-registering registry hooks relocated out of their component headers
// (B1-1279/B1-1314) -- a component composing bb_system for its
// reboot-reason SSOT (etc.) must not also be forced to expose POST
// /api/reboot. `component=` (B1-1275) pulls in each component's own
// REQUIRES/PRIV_REQUIRES closure so the manifest entry composes correctly.
// See bb_wifi_prov_autoinit below for the precedent this mirrors.

// bbtool:init tier=regular fn=bb_system_routes_init server=true component=bb_system

// bbtool:init tier=regular fn=bb_health_init server=true component=bb_health

// bbtool:init tier=regular fn=bb_openapi_init server=true component=bb_openapi

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

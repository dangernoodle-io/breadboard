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
#include "bb_wifi_prov.h"
#include "bb_wifi_prov_default_form.h"

// bbtool:init tier=regular fn=bb_wifi_prov_autoinit args=bb_wifi_prov_default_form_get(),1,NULL provides=http_wildcard_last

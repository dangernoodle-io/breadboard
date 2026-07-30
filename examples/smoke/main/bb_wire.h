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
// registered its own routes for the identical reason). In smoke's
// codegen-composed world, every other component's HTTP routes are
// registered by `tier=regular server=true` entries (bb_wifi_http,
// bb_ota_pull, bb_diag_http, bb_sensor_http, etc.) -- placing this entry in
// tier=regular, with no explicit order=, is sufficient to guarantee it runs
// LAST among them: `bbtool codegen` appends manifest entries after every
// component entry (commands/codegen.py) and wire_graph.topo_sort's tie-break
// is (order ascending, then parse-index ascending) -- an entry with no
// order= sorts after every entry that has one, and among same-order (both
// unset -> infinite) entries the one with the highest parse-index (this
// manifest entry, always parsed last) sorts last. No requires=http_server is
// needed either: tier ordering alone already guarantees pre_http completes
// before any regular-tier entry runs (wire_graph.py's docstring), the same
// convention every other regular-tier server=true entry already relies on.
//
// WARNING: if a second manifest entry is added to this file, manifest entries
// tie-break by parse order (line order), so the captive-portal entry must
// remain textually last, OR both entries must be disambiguated with explicit
// order= values.
#include "bb_wifi_prov.h"
#include "bb_wifi_prov_default_form.h"

// bbtool:init tier=regular fn=bb_wifi_prov_autoinit args=bb_wifi_prov_default_form_get(),1,NULL

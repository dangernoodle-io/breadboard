# bb_serialize: PRIV_REQUIRES -- bb_wifi_prov_scan_wire.c (POST /api/wifi/scan
# response, B1-809 portal) uses bb_serialize_field_t/bb_serialize_desc_t
# directly. Already reachable transitively via bb_http_server (whose public
# bb_http_serialize_stream.h REQUIRES bb_serialize_json, which itself
# REQUIRES bb_serialize) -- listed explicitly per this repo's direct-deps-only
# convention rather than relying on that transitive path.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bbtool.cmake")

# B1-1255: default provisioning form folded in from the former (never-
# approved) bb_prov_default_form component. The getter + generated gz blob
# stay in their own TUs (default_form/bb_wifi_prov_default_form.c + the
# bb_embed_assets-generated source), separate from every TU below that links
# unconditionally -- ESP-IDF components are static archives, so the linker
# only pulls an object file when something resolves an undefined symbol
# against it. A consumer that never calls bb_wifi_prov_default_form_get()
# pays zero flash for the form. See lint rule prov-default-form-internal-ref,
# which forbids any reference to that getter or gz symbol from inside this
# component outside the form's own TU -- that reference would defeat the
# above and hard-link the blob into every consumer image.
#
# The form's TU also lives OUTSIDE src/ (its own default_form/ subdir,
# not src/ or the component root) deliberately: scripts/bbtool/boards.py's
# derive_component() globs *.c non-recursively directly under src/ and under
# the component root for the host/native scaffold build (bb_wifi_prov is in
# [capability.core], so `make test`'s native env always compiles that glob
# unconditionally, unlike ESP-IDF's per-object archive pruning) -- a
# same-directory placement would pull the getter's TU into every host build
# too, and the generated gz blob doesn't exist there (bb_embed_assets only
# runs under ESP-IDF's CMake configure step), breaking the link.
bb_embed_assets(
    OUT_SRCS _embed_srcs
    ASSETS
        default_form/prov_default_form.html:bb_wifi_prov_default_form_gz
)

idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_wifi_prov_parse.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_wifi_prov_page.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_wifi_prov_scan_wire.c"
         "${CMAKE_CURRENT_LIST_DIR}/default_form/bb_wifi_prov_default_form.c"
         ${_embed_srcs}
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_prov/bb_wifi_prov.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_wifi_prov/wifi_prov_mgr.c"
         "${CMAKE_CURRENT_LIST_DIR}/wifi_prov_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_log bb_settings bb_fsm bb_wifi_ap bb_task bb_wifi bb_serialize
)

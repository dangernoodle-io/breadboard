# bb_diag + bb_serialize: REQUIRES -- include/bb_ring_diag.h (the "rings"
# bb_diag section, B1-1077 PR-3a) includes bb_diag_section.h
# (bb_diag_fill_fn/bb_diag_section_t) and bb_serialize.h (bb_serialize_desc_t)
# directly. bb_http_server/esp_http_server stay dropped -- the prior
# hand-rolled exact route (and its bb_http_reserve_routes call) is deleted;
# GET /api/diag/rings is now served by bb_diag's generic wildcard dispatcher.
# bb_http: PRIVATE, re-added (B1-1180 PR-1 review fix) -- bb_ring_diag.c
# defines a PRODUCER-OWNED `static const bb_route_t` describe-only route
# (bb_route_t lives in bb_http.h, the lean route-descriptor-only component)
# wired into the "rings" section's bb_diag_section_t.describe_route;
# bb_diag_section.h itself stays bb_http-free (describe_route is an opaque
# `const void *` there) so this is source-file-only, private.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/bb_ring_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_queue bb_core bb_diag bb_serialize
    PRIV_REQUIRES bb_str bb_http)

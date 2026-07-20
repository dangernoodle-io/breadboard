# bb_diag + bb_serialize: REQUIRES -- include/bb_ring_diag.h (the "rings"
# bb_diag section, B1-1077 PR-3a) includes bb_diag_section.h
# (bb_diag_fill_fn/bb_diag_section_t) and bb_serialize.h (bb_serialize_desc_t)
# directly. bb_http/bb_http_server/esp_http_server are no longer needed --
# the prior hand-rolled exact route (and its bb_http_reserve_routes call) is
# deleted; GET /api/diag/rings is now served by bb_diag's generic wildcard
# dispatcher.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/bb_ring_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_queue bb_core bb_diag bb_serialize
    PRIV_REQUIRES bb_str)

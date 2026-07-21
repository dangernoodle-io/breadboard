idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_storage_http/bb_storage_http_routes.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_storage_http_delete_wire.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_storage
    PRIV_REQUIRES bb_data bb_http bb_http_server bb_json bb_openapi bb_serialize bb_settings bb_log bb_str bb_system bb_timer
)

# bb_storage_http_routes.c is portable (no ESP-IDF-specific includes) but
# filed under platform/espidf/ (mirrors the old bb_nv_delete_routes.c
# location it replaces) -- the host scaffold's directory-convention source
# walk (scripts/bbtool/boards.py's derive_component) only looks under
# platform/host/<name>/ for the "native" board, so this hint pulls the
# portable route source into host test builds too (same pattern as bb_nv's
# three source hints).
# bbtool-scaffold-hint: source=platform/espidf/bb_storage_http/bb_storage_http_routes.c

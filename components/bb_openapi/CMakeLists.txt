idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_openapi_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_openapi/bb_openapi_register.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_http_server bb_json
    PRIV_REQUIRES bb_log bb_system bb_str
)

# bb_openapi_validate.c (B1-1151) is a host-test-only JSON Schema validator --
# zero production callers, so it never ships in firmware and its 31 cJSON
# call sites (and the "json" PRIV_REQUIRES they needed) come out of the
# ESP-IDF build entirely. It is the load-bearing oracle for
# test_route_fidelity.c / test_sse_schema_fidelity.c, so it isn't deleted --
# filed under platform/host/bb_openapi/ instead. No bbtool-scaffold-hint is
# needed: bbtool's board-scaffold discovery globs *.c directly under a
# component's platform/<platform>/<name>/ directory from directory structure
# alone (scripts/bbtool/discovery.py _scan_root / boards.py derive_component),
# so the host build picks this file up automatically, same as any other
# platform-specific TU.

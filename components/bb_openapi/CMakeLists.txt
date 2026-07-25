idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_openapi_emit.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_openapi_emit_shared.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_openapi/bb_openapi_register.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http bb_http_server
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
#
# bb_openapi_emit_tree.c (B1-1116) -- the host-only tree emitter
# (bb_openapi_emit()) -- follows the exact same precedent: zero device
# callers (the device path is bb_openapi_emit_stream(), bb_openapi_emit.c
# above, which never calls bb_json_*), but it IS the load-bearing entrypoint
# for host_tools/emit_openapi and the host test suites that assert against a
# fully-materialized document. Relocated to platform/host/bb_openapi/
# alongside bb_openapi_validate.c, picked up by host builds the same way, and
# never linked into firmware -- which is what lets bb_json (and the cJSON
# backend it pulls in on ESP-IDF) come out of REQUIRES above entirely.

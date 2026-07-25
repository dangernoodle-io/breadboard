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
# bb_openapi_emit_tree.c (B1-1116) was the host-only tree emitter
# (bb_openapi_emit()) -- it followed the exact same precedent: zero device
# callers (the device path is bb_openapi_emit_stream(), bb_openapi_emit.c
# above, which never calls bb_json_*), but it WAS the load-bearing entrypoint
# for host_tools/emit_openapi and the host test suites that asserted against
# a fully-materialized document, so it was relocated to platform/host/bb_openapi/
# alongside bb_openapi_validate.c rather than deleted outright at the time.
# It has since been removed entirely (B1-1054) now that bb_openapi_emit_stream()
# has reached full parity; host_tools/emit_openapi was deleted outright in
# the same change (not migrated), and the host test suites now go through
# bb_openapi_emit_stream() + test_openapi_capture() instead, which is what
# lets bb_json (and the cJSON backend it pulls in on ESP-IDF) come out of
# REQUIRES above entirely.

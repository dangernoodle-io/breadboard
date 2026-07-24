# bb_serialize_meta -- device-shippable engine + co-located table companion
# type for the runtime-from-meta OpenAPI cutover (B1-1059/B1-1181a, this PR).
# Relocated from host_tools/bb_serialize_meta/ (a plain host-only CMake
# STATIC lib outside bbtool's discovery.py component/board scan) into a real
# ESP-IDF component so the engine + tables CAN compile into firmware --
# gated OFF by default via CONFIG_BB_OPENAPI_RUNTIME_META
# (components/bb_openapi/Kconfig, default n): with the config off, SRCS is
# empty and this component contributes zero object bytes to the device
# image (byte-identical to before this PR). The PlatformIO native host test
# env always ships it regardless of this Kconfig (BB_SERIALIZE_META_HOST=1,
# unchanged from its host_tools-era behavior) -- see bb_serialize_meta.h's
# BB_SERIALIZE_META_SHIP gate, which every co-located meta table in
# components/<name>/ is now guarded on. Nothing calls the engine on-device
# yet (that's a later PR) -- this PR is purely "can it ship", inert.
set(_bb_serialize_meta_srcs)
if(CONFIG_BB_OPENAPI_RUNTIME_META)
    list(APPEND _bb_serialize_meta_srcs
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_serialize_meta_openapi.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_serialize_meta_validate.c")
endif()

idf_component_register(
    SRCS ${_bb_serialize_meta_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_serialize bb_core
)

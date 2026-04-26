cmake_minimum_required(VERSION 3.16)
project(emit_openapi C)

set(CMAKE_C_STANDARD 11)

# Anchor breadboard root relative to this file's location
set(BB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# Find cJSON — prefer a system install; fall back to FetchContent
find_package(cJSON QUIET)
if(NOT cJSON_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG        v1.7.18
    )
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cjson)
    set(CJSON_TARGET cjson)
else()
    set(CJSON_TARGET cJSON::cJSON)
endif()

add_executable(emit_openapi
    "${CMAKE_CURRENT_LIST_DIR}/main.c"
    "${BB_ROOT}/components/bb_openapi/src/bb_openapi_emit.c"
    "${BB_ROOT}/platform/espidf/bb_json/bb_json_cjson.c"
    "${BB_ROOT}/components/bb_http/src/route_registry.c"
    "${BB_ROOT}/components/bb_http/src/http_utils.c"
    "${BB_ROOT}/platform/host/bb_http/bb_http_host.c"
)

target_include_directories(emit_openapi PRIVATE
    "${BB_ROOT}/components/bb_openapi/include"
    "${BB_ROOT}/components/bb_http/include"
    "${BB_ROOT}/components/bb_json/include"
    "${BB_ROOT}/components/bb_log/include"
    "${BB_ROOT}/components/bb_nv/include"
)

target_compile_definitions(emit_openapi PRIVATE
    BB_OPENAPI_HARNESS_FIXTURES
)

target_link_libraries(emit_openapi PRIVATE ${CJSON_TARGET})

# When using FetchContent, cJSON headers aren't automatically on the include path
# for non-target-based includes; add them explicitly.
if(NOT cJSON_FOUND)
    target_include_directories(emit_openapi PRIVATE
        "${cjson_SOURCE_DIR}"
    )
endif()

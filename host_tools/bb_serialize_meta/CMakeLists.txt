cmake_minimum_required(VERSION 3.16)
project(bb_serialize_meta C)

set(CMAKE_C_STANDARD 11)

# Anchor breadboard root relative to this file's location
set(BB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# Plain CMake STATIC library -- host-only, never idf_component_register,
# must never enter the device image (see bb_serialize_meta.h banner: this
# tree is outside bbtool's discovery.py component/board scan). Consumers
# (host_tools/emit_openapi, host CMake test drivers) add_subdirectory() this
# and link against the `bb_serialize_meta` target below.
add_library(bb_serialize_meta STATIC
    "${CMAKE_CURRENT_LIST_DIR}/bb_serialize_meta_openapi.c"
    "${CMAKE_CURRENT_LIST_DIR}/bb_serialize_meta_validate.c"
)

target_include_directories(bb_serialize_meta PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${BB_ROOT}/components/bb_core/include"
    "${BB_ROOT}/components/bb_serialize/include"
)

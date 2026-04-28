# Force-keep a bb_registry-registered component's .o under linkers that
# garbage-collect translation units with no external symbol references.
# Required because PlatformIO's espidf builder strips ESP-IDF's WHOLE_ARCHIVE
# flag, leaving constructor-only .o files at the mercy of --gc-sections.
#
# Usage in a component's CMakeLists.txt, after idf_component_register():
#   include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
#   bb_registry_force_register(${COMPONENT_LIB} bb_ota_pull)
function(bb_registry_force_register comp_lib name)
    target_link_libraries(${comp_lib} INTERFACE "-u bb_registry_register__${name}")
endfunction()

# Force-keep a bb_registry early-tier-registered component's .o under linkers that
# garbage-collect translation units with no external symbol references.
# Same rationale as bb_registry_force_register, but for the early-tier API.
#
# Usage in a component's CMakeLists.txt, after idf_component_register():
#   include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_registry.cmake")
#   bb_registry_force_register_early(${COMPONENT_LIB} bb_log_stream)
function(bb_registry_force_register_early comp_lib name)
    target_link_libraries(${comp_lib} INTERFACE "-u bb_registry_register_early__${name}")
endfunction()

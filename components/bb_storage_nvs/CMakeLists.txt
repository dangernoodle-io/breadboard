idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_storage_nvs/bb_storage_nvs.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_storage_nvs_get_decision.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_storage_nvs_classify_enc.c"
         "${CMAKE_CURRENT_LIST_DIR}/src/bb_storage_nvs_classify_nvs_type.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core
    PRIV_REQUIRES bb_storage bb_log bb_str nvs_flash
)

# bb_storage_nvs's real implementation is ESP-IDF-only (wraps nvs.h), but the
# same file also ships a host-stub branch (#else block) so the component
# compiles unchanged on host per breadboard's portability rule -- filed under
# platform/espidf/ only, mirroring bb_nv's own platform/espidf-only filing
# (bb_nv.c has the identical ESP_PLATFORM/host-stub split).
# bbtool-scaffold-hint: source=platform/espidf/bb_storage_nvs/bb_storage_nvs.c

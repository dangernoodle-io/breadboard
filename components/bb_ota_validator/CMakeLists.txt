# bb_serialize_meta: PRIV_REQUIRES -- co-located bb_serialize_desc_meta_t table(s) in this component's own .c files, gated on BB_SERIALIZE_META_SHIP (B1-1059 device-shippable PR; default off via CONFIG_BB_OPENAPI_RUNTIME_META).
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_ota_validator/bb_ota_validator.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_ota_validator_partitions_wire.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_http_server
    PRIV_REQUIRES bb_http bb_log app_update bb_system bb_serialize bb_serialize_meta
)

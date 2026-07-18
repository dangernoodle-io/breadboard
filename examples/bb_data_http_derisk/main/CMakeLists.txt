# THROWAWAY DE-RISK EXAMPLE -- see ../CMakeLists.txt. Hand-literal REQUIRES
# (no codegen/autowire fragment): this example is deleted wholesale at the
# B1-1045 cutover, so a generated-fragment dependency would be pure churn.
idf_component_register(
    SRCS "derisk_app.c"
    INCLUDE_DIRS "."
    REQUIRES bb_log bb_core bb_data bb_data_http bb_http_server
             bb_serialize_json bb_timer bb_wifi bb_storage_nvs
             bb_lifecycle bb_meminfo)

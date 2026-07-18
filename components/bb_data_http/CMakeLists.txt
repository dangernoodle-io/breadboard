# bb_data_http's pure core has no FreeRTOS/httpd/bb_data/bb_ws_server
# dependency of its own -- render/generation/send are injected fn-pointer
# seams the composition root wires up. REQUIRES stays bb_queue + bb_core
# only; bb_registry is PRIV_REQUIRES (internal attach-table storage, not part
# of the public API surface).
idf_component_register(
    SRCS "src/bb_data_http_common.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_queue bb_core
    PRIV_REQUIRES bb_log bb_registry bb_str)

idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES bb_core bb_serialize
    PRIV_REQUIRES bb_registry
)

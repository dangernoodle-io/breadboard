idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_fan bb_power bb_temp
)

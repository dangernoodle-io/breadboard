set(BB_BQUEUE_PRIV_REQUIRES) # empty CMake list -- NOT set(... "") which is a
                              # one-element list containing an empty string
                              # and corrupts docs generation's REQUIRES parse

if(IDF_TARGET)
    set(BB_BQUEUE_BACKEND
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_bqueue/bb_bqueue.c")
    list(APPEND BB_BQUEUE_PRIV_REQUIRES freertos)
else()
    set(BB_BQUEUE_BACKEND
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_bqueue/bb_bqueue.c")
endif()

idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/bb_bqueue_common.c"
        "${BB_BQUEUE_BACKEND}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bb_core
    PRIV_REQUIRES ${BB_BQUEUE_PRIV_REQUIRES})

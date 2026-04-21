idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_prov_default_form.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_http
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/prov_default_form.html"
)

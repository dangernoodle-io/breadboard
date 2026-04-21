set(_GZ_C "${CMAKE_CURRENT_BINARY_DIR}/prov_default_form_gz.c")

add_custom_command(
    OUTPUT  "${_GZ_C}"
    COMMAND python3
            "${CMAKE_CURRENT_LIST_DIR}/../../scripts/embed_html.py"
            "${CMAKE_CURRENT_LIST_DIR}/prov_default_form.html"
            "${_GZ_C}"
            bb_prov_default_form_gz
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/prov_default_form.html"
            "${CMAKE_CURRENT_LIST_DIR}/../../scripts/embed_html.py"
)

idf_component_register(
    SRCS "src/bb_prov_default_form.c" "${_GZ_C}"
    INCLUDE_DIRS "include"
    REQUIRES bb_http
)

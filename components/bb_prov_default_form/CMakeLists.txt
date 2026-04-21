include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bb_embed.cmake")

bb_embed_assets(
    OUT_SRCS _embed_srcs
    ASSETS
        prov_default_form.html:bb_prov_default_form_gz
)

idf_component_register(
    SRCS "src/bb_prov_default_form.c" ${_embed_srcs}
    INCLUDE_DIRS "include"
    REQUIRES bb_http
)

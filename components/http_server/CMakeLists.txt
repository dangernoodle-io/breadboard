idf_component_register(
    SRCS "src/http_server.c" "src/http_utils.c" "src/prov_form_html_gz.c" "src/theme_css_gz.c" "src/logo_svg_gz.c" "src/favicon_svg_gz.c" "src/prov_save_html_gz.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_config esp_http_server
)

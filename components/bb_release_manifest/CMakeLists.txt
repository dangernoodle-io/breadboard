idf_component_register(
    SRCS "src/bb_release_manifest_github.c"
          "src/bb_release_manifest_github_stream.c"
          "src/bb_release_manifest_json_sink.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_serialize_json
)

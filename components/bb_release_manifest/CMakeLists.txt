idf_component_register(
    SRCS "src/bb_release_manifest_github.c"
          "src/bb_release_manifest_github_stream.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
)

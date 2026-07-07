# FOUNDATIONAL FLOOR (jae/floor-handwire, decision #724): HAND-WIRE mode --
# floor_app.c calls bb_log_stream_init()/bb_log_config_init() directly and no
# longer touches the bb_init walker (bb_init_init_early()), so main does NOT
# REQUIRES bb_init itself. bb_log no longer PRIV_REQUIRES bb_init either
# (self-registration machinery -- BB_INIT_REGISTER_EARLY, the -u force-keeps,
# CONFIG_BB_LOG_*_AUTOREGISTER -- was removed from bb_log/bb_log_config), so
# bb_init is fully out of the floor's link closure.
#
# main REQUIRES only the composed seeds -- bb_log's, bb_meminfo's, and
# bb_timer's OWN
# CMakeLists.txt are evaluated for real by ESP-IDF's actual cmake config step
# (real Kconfig values from sdkconfig), so the true, Kconfig-pruned
# transitive closure is what actually gets linked. The generated fragment is
# only consulted by the top-level CMakeLists.txt for the (over-inclusive,
# harmless) COMPONENTS discovery allowlist.
idf_component_register(
    SRCS "floor_app.c"
    INCLUDE_DIRS "."
    REQUIRES bb_log bb_meminfo bb_timer)

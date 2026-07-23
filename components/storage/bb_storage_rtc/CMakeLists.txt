idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../platform/espidf/bb_storage_rtc/bb_storage_rtc.c"
         "${CMAKE_CURRENT_LIST_DIR}/bb_storage_rtc_region.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
    PRIV_REQUIRES bb_storage bb_log
)

# bb_storage_rtc's ESP-IDF vtable impl (wraps RTC_NOINIT_ATTR) lives under
# platform/espidf/ and is wired into SRCS above for the real ESP-IDF build.
# A SEPARATE host stand-in (a plain static, no RTC_NOINIT) lives at
# platform/host/bb_storage_rtc/bb_storage_rtc.c, implementing the same
# vtable contract off-device -- the native test build's scaffold-derived
# source graph (custom_bb_board=native) picks that file up on its own via
# the platform/host/<name>/ directory convention, independent of this SRCS
# list, mirroring bb_storage_ram/bb_storage_nvs's host/espidf split.
# bb_storage_rtc_region.c (the pure pack/valid/crc logic) is portable and
# compiles unchanged on both platforms.

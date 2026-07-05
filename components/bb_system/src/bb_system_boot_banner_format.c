#include "bb_system.h"

#include <stdio.h>

int bb_system_boot_banner_format(char *out, size_t out_len,
                                  const char *project, const char *version,
                                  const char *build_date, const char *build_time,
                                  const char *idf_version)
{
    if (!out || out_len == 0) return -1;

    project = project ? project : "?";
    version = version ? version : "?";
    build_date = build_date ? build_date : "?";
    build_time = build_time ? build_time : "?";
    idf_version = idf_version ? idf_version : "?";

    return snprintf(out, out_len, "project=%s version=%s build=%s %s idf=%s",
                     project, version, build_date, build_time, idf_version);
}

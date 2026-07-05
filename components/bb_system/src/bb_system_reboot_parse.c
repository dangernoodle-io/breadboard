// Pure (host-testable) parse of POST /api/reboot's optional JSON body.
// No platform types — only bb_json (already portable: cJSON on
// ESP-IDF/host, ArduinoJson wrapper on Arduino). Compiled on host, ESP-IDF,
// and Arduino.
#include "bb_system.h"

#include "bb_json.h"
#include "bb_str.h"

#include <stdint.h>
#include <string.h>

void bb_system_reboot_parse_body(const char *body, int body_len, const char *ua_or_null,
                                 uint32_t *out_ts, char *out_detail, size_t out_detail_len)
{
    if (!out_ts || !out_detail || out_detail_len == 0U) return;

    uint32_t ts = 0;
    char detail[49] = {0};

    if (body && body_len > 0) {
        bb_json_t parsed = bb_json_parse(body, (size_t)body_len);
        if (parsed) {
            double ts_num = 0;
            if (bb_json_obj_get_number(parsed, "ts", &ts_num) &&
                ts_num > 0.0 && ts_num <= (double)UINT32_MAX) {
                ts = (uint32_t)ts_num;
            }
            bb_json_obj_get_string(parsed, "detail", detail, sizeof(detail));
            bb_json_free(parsed);
        }
    }

    // Precedence: body detail (non-empty) > ua_or_null (non-NULL/non-empty) > "".
    if (!detail[0] && ua_or_null && ua_or_null[0]) {
        bb_strlcpy(detail, ua_or_null, sizeof(detail));
    }

    *out_ts = ts;
    size_t n = strnlen(detail, sizeof(detail));
    if (n >= out_detail_len) n = out_detail_len - 1;
    memcpy(out_detail, detail, n);
    out_detail[n] = '\0';
}

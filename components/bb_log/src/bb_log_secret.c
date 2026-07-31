// bb_log_secret — the single sanctioned entry point for putting a
// secret-derived value (WiFi/MQTT password, PSK, token, ...) into a log
// line, masked. Pure/portable: no platform headers, compiled on every
// backend via the bb_log_e/w/i/d/v macros bb_log.h already defines per
// platform, so the masking policy is identical everywhere. See bb_log.h's
// doc comment for the call contract.
#include "bb_log.h"

void bb_log_secret(bb_log_level_t level, const char *tag, const char *label, const char *secret)
{
    // tag/label are forwarded into a printf-style "%s" below -- normalize
    // NULL to "?" here so a NULL tag/label can never reach the underlying
    // vprintf-family call (UB on NULL per the C standard; glibc happens to
    // print "(null)", other libcs may crash). Mirrors the doc comment in
    // bb_log.h.
    tag = tag ? tag : "?";
    label = label ? label : "?";

    // secret's bytes are read here ONLY to classify unset-vs-set -- never
    // passed to a printf-style format, so there is no vector for the raw
    // value to reach the log through this function.
    const char *value = (secret && secret[0]) ? BB_LOG_SECRET_MASK : "(unset)";

    switch (level) {
    case BB_LOG_LEVEL_ERROR:
        bb_log_e(tag, "%s=%s", label, value);
        break;
    case BB_LOG_LEVEL_WARN:
        bb_log_w(tag, "%s=%s", label, value);
        break;
    case BB_LOG_LEVEL_INFO:
        bb_log_i(tag, "%s=%s", label, value);
        break;
    case BB_LOG_LEVEL_DEBUG:
        bb_log_d(tag, "%s=%s", label, value);
        break;
    case BB_LOG_LEVEL_VERBOSE:
        bb_log_v(tag, "%s=%s", label, value);
        break;
    case BB_LOG_LEVEL_NONE:
    default:
        break;
    }
}

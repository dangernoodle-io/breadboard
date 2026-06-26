#!/bin/sh
# check_lint.sh — consolidated forbidden-pattern lint for breadboard.
#
# Enforces four checks in sequence; exits non-zero on any failure.
#
# Check 1: deprecated-http-send (B1-241)
#   bb_http_resp_send_json( / bb_http_resp_send_err( / bb_http_resp_send( are
#   buffered-response APIs superseded by the streaming chunk API. Scans
#   components/ only; platform/ holds the backend implementations themselves.
#
# Check 2: public-header-leak (B1-263)
#   Public headers under components/*/include/ must not include esp_*.h or
#   driver/*.h outside an #ifdef ESP_PLATFORM guard. Uses awk to track gate
#   depth line-by-line; includes inside a gate pass, ungated ones fail.
#   Exempt: bb_display_ek79007 (LVGL escape hatch; see CLAUDE.md Display
#           section — esp_lvgl_port exposes lv_obj_t* deliberately).
#
# Check 3: state-topic-post guard (B1-356)
#   Canonical state topics must flow through bb_cache; direct bb_event_post()
#   calls outside bb_cache or test/ are regressions.
#
# Check 4: public-requires-watchlist (B1-263)
#   High-risk ESP-IDF deps must be PRIV_REQUIRES unless the component:dep pair
#   is in the allowlist (see ALLOWLIST section below).
#
# Allowlist — component:dep pairs (granular; no whole-component blankets):
#
#   PERMANENT:
#     bb_display_ek79007 : esp_lvgl_port   LVGL large-panel escape hatch,
#                                          documented in CLAUDE.md Display section.
#     bb_display_ek79007 : lv_             same LVGL escape hatch.
#     bb_display_ek79007 : lvgl            same LVGL escape hatch.
#
#   TEMPORARY DEBT (remove when B1-42/B1-344 migrate to bb_i2c_dev_*):
#     bb_display_ssd1306 : esp_driver_i2c  exposes i2c_master_dev_handle_t in
#                                          gated public header.
#     bb_fan_emc2101     : esp_driver_i2c  same temporary-debt comment.
#     bb_power_tps546    : esp_driver_i2c  same temporary-debt comment.

# Allow tests to override REPO_ROOT via environment variable.
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"

FAIL=0

# ---------------------------------------------------------------------------
# Check 1: deprecated-http-send (B1-241)
# Scans components/ only (platform/ holds the backend implementations).
# ---------------------------------------------------------------------------
DEPRECATED_PATTERN='bb_http_resp_send_json\(|bb_http_resp_send_err\(|bb_http_resp_send\('

SEND_VIOLATIONS="$(grep -rn \
    --include="*.c" --include="*.h" \
    --exclude-dir=".pio" \
    --exclude-dir=".claude" \
    -E "${DEPRECATED_PATTERN}" \
    "${REPO_ROOT}/components/" \
    2>/dev/null \
    | grep -v "bb_http_resp_send_chunk\|bb_http_resp_sendstr" \
    || true)"

if [ -n "${SEND_VIOLATIONS}" ]; then
    echo "${SEND_VIOLATIONS}" | while IFS= read -r match; do
        file="${match%%:*}"
        rest="${match#*:}"
        lineno="${rest%%:*}"
        echo "ERROR [deprecated-http-send]: ${file}:${lineno}"
    done
    count="$(echo "${SEND_VIOLATIONS}" | wc -l | tr -d ' ')"
    echo "check_lint [deprecated-http-send]: ${count} violation(s) — use bb_http_resp_send_chunk / bb_http_resp_sendstr" >&2
    FAIL=1
fi

# ---------------------------------------------------------------------------
# Check 2: public-header-leak (B1-263)
# Gate-aware: awk tracks #ifdef ESP_PLATFORM depth. Includes inside a gate pass.
# Exempt: bb_display_ek79007 (LVGL large-panel escape hatch).
# ---------------------------------------------------------------------------
HEADER_VIOLATIONS="$(
    find "${REPO_ROOT}/components" -path "*/include/*.h" \
        -not -path "*/bb_display_ek79007/*" \
    | sort \
    | xargs awk '
        BEGIN { gate = 0 }
        /^[[:space:]]*#[[:space:]]*(ifdef[[:space:]]+ESP_PLATFORM|if[[:space:]]+defined\([[:space:]]*ESP_PLATFORM[[:space:]]*\))/ {
            gate++; next
        }
        /^[[:space:]]*#[[:space:]]*(elif|else)/ { if (gate > 0) gate--; next }
        /^[[:space:]]*#[[:space:]]*endif/ { if (gate > 0) gate--; next }
        /^[[:space:]]*#[[:space:]]*include/ {
            if (gate == 0 &&
                (/["<](esp_|freertos\/)/ || /["<]driver\// || /["<]cJSON\.h/)) {
                print FILENAME ":" NR ": " $0
            }
        }
    ' 2>/dev/null \
    || true
)"

if [ -n "${HEADER_VIOLATIONS}" ]; then
    echo "${HEADER_VIOLATIONS}" | while IFS= read -r match; do
        echo "ERROR [public-header-leak]: ${match}"
    done
    count="$(echo "${HEADER_VIOLATIONS}" | wc -l | tr -d ' ')"
    echo "check_lint [public-header-leak]: ${count} violation(s) — gate esp_ includes with #ifdef ESP_PLATFORM" >&2
    FAIL=1
fi

# ---------------------------------------------------------------------------
# Check 3: state-topic-post guard (B1-356)
# ---------------------------------------------------------------------------
TOPIC_TOKENS='BB_NET_HEALTH_TOPIC|BB_DIAG_BOOT_TOPIC|BB_UPDATE_CHECK_TOPIC|BB_DISPLAY_INFO_TOPIC|"net\.health"|"diag\.boot"|"update\.available"|"health\.display"'

STATE_VIOLATIONS="$(grep -rn "bb_event_post(" \
    --include="*.c" --include="*.h" \
    --exclude-dir=".pio" \
    --exclude-dir=".claude" \
    "${REPO_ROOT}" \
    2>/dev/null \
    | grep -E "${TOPIC_TOKENS}" \
    | grep -v "^${REPO_ROOT}/platform/espidf/bb_cache/" \
    | grep -v "^${REPO_ROOT}/platform/host/bb_cache/" \
    | grep -v "^${REPO_ROOT}/components/bb_cache/" \
    | grep -v "^${REPO_ROOT}/test/" \
    || true)"

if [ -n "${STATE_VIOLATIONS}" ]; then
    echo "${STATE_VIOLATIONS}" | while IFS= read -r match; do
        file="${match%%:*}"
        rest="${match#*:}"
        lineno="${rest%%:*}"
        echo "ERROR [state-topic-post]: ${file}:${lineno}"
    done
    count="$(echo "${STATE_VIOLATIONS}" | wc -l | tr -d ' ')"
    echo "check_lint [state-topic-post]: ${count} violation(s) — state topics must be posted through bb_cache" >&2
    FAIL=1
fi

# ---------------------------------------------------------------------------
# Check 4: public-requires-watchlist (B1-263)
#
# Watchlist: these must be PRIV_REQUIRES unless allowlisted.
WATCHLIST="esp_driver_ esp_lcd esp_http_server esp_timer esp_system app_update espressif__mdns"
#
# Allowlist: component_name|dep_prefix pairs.
# A pair matches when basename(dir)==component AND dep starts with dep_prefix.
#
# PERMANENT:
#   bb_display_ek79007|esp_lvgl_port  LVGL large-panel escape hatch, documented
#                                     in CLAUDE.md Display section.
#   bb_display_ek79007|lv_            same LVGL escape hatch (lvgl headers).
#   bb_display_ek79007|lvgl           same LVGL escape hatch (lvgl component).
#
# TEMPORARY DEBT (remove when B1-42/B1-344 migrate to bb_i2c_dev_*):
#   bb_display_ssd1306|esp_driver_i2c  exposes i2c_master_dev_handle_t in gated
#                                      public header.
#   bb_fan_emc2101|esp_driver_i2c      same temporary-debt comment.
#   bb_power_tps546|esp_driver_i2c     same temporary-debt comment.
# ---------------------------------------------------------------------------
check_allowlist() {
    _comp="$1"
    _dep="$2"
    _key="${_comp}:${_dep}"
    case "${_key}" in
        "bb_display_ek79007:esp_lvgl_port"*) return 0 ;;
        "bb_display_ek79007:lv_"*)           return 0 ;;
        "bb_display_ek79007:lvgl"*)          return 0 ;;
        "bb_display_ssd1306:esp_driver_i2c"*) return 0 ;;
        "bb_fan_emc2101:esp_driver_i2c"*)    return 0 ;;
        "bb_power_tps546:esp_driver_i2c"*)   return 0 ;;
    esac
    return 1
}

is_watchlist() {
    _dep="$1"
    for _w in ${WATCHLIST}; do
        case "${_dep}" in
            "${_w}"*) return 0 ;;
        esac
    done
    return 1
}

REQUIRES_VIOLATIONS=""

for cmake_file in $(find "${REPO_ROOT}/components" -name "CMakeLists.txt" | sort); do
    comp="$(basename "$(dirname "${cmake_file}")")"
    # Extract deps from REQUIRES lines, skipping PRIV_REQUIRES.
    # We read lines that contain a bare REQUIRES keyword (not preceded by PRIV_).
    while IFS= read -r line; do
        # Skip PRIV_REQUIRES
        case "${line}" in
            *PRIV_REQUIRES*) continue ;;
        esac
        # Only process lines with REQUIRES
        case "${line}" in
            *REQUIRES*) ;;
            *) continue ;;
        esac
        # Strip up to and including REQUIRES keyword, strip trailing )
        deps="${line#*REQUIRES}"
        deps="${deps%%)*}"
        for dep in ${deps}; do
            case "${dep}" in
                REQUIRES|idf_component_register) continue ;;
            esac
            if is_watchlist "${dep}"; then
                if ! check_allowlist "${comp}" "${dep}"; then
                    lineno="$(grep -n "${line}" "${cmake_file}" 2>/dev/null | head -1 | cut -d: -f1)"
                    REQUIRES_VIOLATIONS="${REQUIRES_VIOLATIONS}${cmake_file}:${lineno}: component=${comp} dep=${dep}"$'\n'
                fi
            fi
        done
    done < "${cmake_file}"
done

if [ -n "${REQUIRES_VIOLATIONS}" ]; then
    echo "${REQUIRES_VIOLATIONS}" | while IFS= read -r match; do
        [ -n "${match}" ] || continue
        echo "ERROR [public-requires-watchlist]: ${match}"
    done
    count="$(printf '%s' "${REQUIRES_VIOLATIONS}" | grep -c '^.' || true)"
    echo "check_lint [public-requires-watchlist]: ${count} violation(s) — move watchlist deps to PRIV_REQUIRES" >&2
    FAIL=1
fi

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------
if [ "${FAIL}" -ne 0 ]; then
    exit 1
fi

echo "check_lint: all checks passed"

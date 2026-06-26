#!/bin/sh
# check_lint_test.sh — self-tests for check_lint.sh
#
# Each test plants a violation or a passing case in a temporary directory,
# runs a targeted sub-check, and asserts the expected exit code + label.
#
# Usage: bash scripts/check_lint_test.sh
# Exit 0 = all assertions passed; exit 1 = at least one assertion failed.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PASS=0
FAIL=0

# Temp workspace — cleaned up on exit
TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "${TMPDIR_WORK}"' EXIT

assert_exit() {
    _label="$1"
    _expected="$2"
    _actual="$3"
    if [ "${_actual}" = "${_expected}" ]; then
        echo "  PASS: ${_label}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${_label} (expected exit ${_expected}, got ${_actual})"
        FAIL=$((FAIL + 1))
    fi
}

assert_output() {
    _label="$1"
    _pattern="$2"
    _output="$3"
    if echo "${_output}" | grep -qE "${_pattern}"; then
        echo "  PASS: ${_label}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${_label} (pattern '${_pattern}' not found in output)"
        echo "    output: ${_output}"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Check 1: deprecated-http-send ==="

# Positive: calling bb_http_resp_send_json( in a component .c fires the label
TMPDIR_C1="${TMPDIR_WORK}/c1"
mkdir -p "${TMPDIR_C1}/components/bb_fake/src"
printf 'bb_err_t foo(bb_http_request_t *r) { return bb_http_resp_send_json(r, doc); }\n' \
    > "${TMPDIR_C1}/components/bb_fake/src/fake.c"

OUT="$(export REPO_ROOT="${TMPDIR_C1}"; sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
assert_output "deprecated-http-send fires on bb_http_resp_send_json(" \
    "deprecated-http-send" "${OUT}"

# Positive: calling bb_http_resp_send_err( fires
TMPDIR_C1E="${TMPDIR_WORK}/c1e"
mkdir -p "${TMPDIR_C1E}/components/bb_fake/src"
printf 'void bar(void) { bb_http_resp_send_err(r, code, msg); }\n' \
    > "${TMPDIR_C1E}/components/bb_fake/src/fake.c"

OUT="$(REPO_ROOT="${TMPDIR_C1E}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
assert_output "deprecated-http-send fires on bb_http_resp_send_err(" \
    "deprecated-http-send" "${OUT}"

# Negative: bb_http_resp_send_chunk( must NOT fire
TMPDIR_C1N="${TMPDIR_WORK}/c1n"
mkdir -p "${TMPDIR_C1N}/components/bb_fake/src"
printf 'void baz(void) { bb_http_resp_send_chunk(r, buf, len); }\n' \
    > "${TMPDIR_C1N}/components/bb_fake/src/fake.c"

OUT="$(REPO_ROOT="${TMPDIR_C1N}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
RC=$?
if echo "${OUT}" | grep -q "deprecated-http-send"; then
    echo "  FAIL: deprecated-http-send must NOT fire on send_chunk"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: deprecated-http-send does not fire on send_chunk"
    PASS=$((PASS + 1))
fi

echo ""
echo "=== Check 2: public-header-leak ==="

# Positive: ungated esp_ include fires
TMPDIR_C2="${TMPDIR_WORK}/c2"
mkdir -p "${TMPDIR_C2}/components/bb_fake/include"
printf '#pragma once\n#include "esp_http_server.h"\n' \
    > "${TMPDIR_C2}/components/bb_fake/include/bb_fake.h"

OUT="$(REPO_ROOT="${TMPDIR_C2}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
assert_output "public-header-leak fires on ungated esp_ include" \
    "public-header-leak" "${OUT}"

# Negative: gated esp_ include passes
TMPDIR_C2N="${TMPDIR_WORK}/c2n"
mkdir -p "${TMPDIR_C2N}/components/bb_fake/include"
printf '#pragma once\n#ifdef ESP_PLATFORM\n#include "esp_http_server.h"\n#endif\n' \
    > "${TMPDIR_C2N}/components/bb_fake/include/bb_fake.h"

OUT="$(REPO_ROOT="${TMPDIR_C2N}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
if echo "${OUT}" | grep -q "public-header-leak"; then
    echo "  FAIL: public-header-leak must NOT fire on gated esp_ include"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: public-header-leak does not fire on gated esp_ include"
    PASS=$((PASS + 1))
fi

# Negative: bb_display_ek79007 exempt even with ungated include
TMPDIR_C2EK="${TMPDIR_WORK}/c2ek"
mkdir -p "${TMPDIR_C2EK}/components/bb_display_ek79007/include"
printf '#pragma once\n#include "esp_lcd.h"\n#include "lvgl.h"\n' \
    > "${TMPDIR_C2EK}/components/bb_display_ek79007/include/bb_display_ek79007.h"

OUT="$(REPO_ROOT="${TMPDIR_C2EK}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
if echo "${OUT}" | grep -q "public-header-leak"; then
    echo "  FAIL: public-header-leak must NOT fire for bb_display_ek79007 (exempt)"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: public-header-leak does not fire for bb_display_ek79007 (exempt)"
    PASS=$((PASS + 1))
fi

echo ""
echo "=== Check 3: state-topic-post ==="

# Positive: bb_event_post with state topic outside bb_cache fires
TMPDIR_C3="${TMPDIR_WORK}/c3"
mkdir -p "${TMPDIR_C3}/components/bb_fake/src"
printf 'void foo(void) { bb_event_post(ev, "net.health", data, len); }\n' \
    > "${TMPDIR_C3}/components/bb_fake/src/fake.c"

OUT="$(REPO_ROOT="${TMPDIR_C3}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
assert_output "state-topic-post fires on bb_event_post with state topic" \
    "state-topic-post" "${OUT}"

# Negative: same call inside bb_cache passes
TMPDIR_C3N="${TMPDIR_WORK}/c3n"
mkdir -p "${TMPDIR_C3N}/platform/espidf/bb_cache"
printf 'void foo(void) { bb_event_post(ev, "net.health", data, len); }\n' \
    > "${TMPDIR_C3N}/platform/espidf/bb_cache/bb_cache.c"

OUT="$(REPO_ROOT="${TMPDIR_C3N}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
if echo "${OUT}" | grep -q "state-topic-post"; then
    echo "  FAIL: state-topic-post must NOT fire inside bb_cache"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: state-topic-post does not fire inside bb_cache"
    PASS=$((PASS + 1))
fi

echo ""
echo "=== Check 4: public-requires-watchlist ==="

# Positive: non-allowlisted watchlist dep in REQUIRES fires
TMPDIR_C4="${TMPDIR_WORK}/c4"
mkdir -p "${TMPDIR_C4}/components/bb_fake"
printf 'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core esp_lcd\n)\n' \
    > "${TMPDIR_C4}/components/bb_fake/CMakeLists.txt"

OUT="$(REPO_ROOT="${TMPDIR_C4}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
assert_output "public-requires-watchlist fires on non-allowlisted dep" \
    "public-requires-watchlist" "${OUT}"

# Negative: same dep in PRIV_REQUIRES passes
TMPDIR_C4N="${TMPDIR_WORK}/c4n"
mkdir -p "${TMPDIR_C4N}/components/bb_fake"
printf 'idf_component_register(\n    SRCS "fake.c"\n    PRIV_REQUIRES bb_core esp_lcd\n)\n' \
    > "${TMPDIR_C4N}/components/bb_fake/CMakeLists.txt"

OUT="$(REPO_ROOT="${TMPDIR_C4N}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
if echo "${OUT}" | grep -q "public-requires-watchlist"; then
    echo "  FAIL: public-requires-watchlist must NOT fire on PRIV_REQUIRES dep"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: public-requires-watchlist does not fire on PRIV_REQUIRES dep"
    PASS=$((PASS + 1))
fi

# Allowlist fixture A: bb_display_ssd1306 + esp_driver_i2c in REQUIRES PASSES
TMPDIR_C4AL="${TMPDIR_WORK}/c4al"
mkdir -p "${TMPDIR_C4AL}/components/bb_display_ssd1306"
printf 'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core bb_display esp_driver_i2c\n)\n' \
    > "${TMPDIR_C4AL}/components/bb_display_ssd1306/CMakeLists.txt"

OUT="$(REPO_ROOT="${TMPDIR_C4AL}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
if echo "${OUT}" | grep -q "public-requires-watchlist"; then
    echo "  FAIL: allowlisted pair (bb_display_ssd1306 / esp_driver_i2c) must pass"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: allowlisted pair (bb_display_ssd1306 / esp_driver_i2c) passes"
    PASS=$((PASS + 1))
fi

# Allowlist fixture B: SAME dep (esp_driver_i2c) on NON-allowlisted component FIRES
TMPDIR_C4NL="${TMPDIR_WORK}/c4nl"
mkdir -p "${TMPDIR_C4NL}/components/bb_fake_i2c"
printf 'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core esp_driver_i2c\n)\n' \
    > "${TMPDIR_C4NL}/components/bb_fake_i2c/CMakeLists.txt"

OUT="$(REPO_ROOT="${TMPDIR_C4NL}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
assert_output "allowlist dep on non-allowlisted component fires" \
    "public-requires-watchlist" "${OUT}"

# Allowlist fixture C: bb_display_ek79007 + esp_lvgl_port in REQUIRES PASSES (permanent)
TMPDIR_C4EK="${TMPDIR_WORK}/c4ek"
mkdir -p "${TMPDIR_C4EK}/components/bb_display_ek79007"
printf 'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core bb_display esp_lvgl_port\n)\n' \
    > "${TMPDIR_C4EK}/components/bb_display_ek79007/CMakeLists.txt"

OUT="$(REPO_ROOT="${TMPDIR_C4EK}" sh "${SCRIPT_DIR}/check_lint.sh" 2>&1 || true)"
if echo "${OUT}" | grep -q "public-requires-watchlist"; then
    echo "  FAIL: allowlisted pair (bb_display_ek79007 / esp_lvgl_port) must pass"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: allowlisted pair (bb_display_ek79007 / esp_lvgl_port) passes"
    PASS=$((PASS + 1))
fi

echo ""
echo "=== Integration: real repo passes clean ==="
OUT="$(bash "${SCRIPT_DIR}/check_lint.sh" 2>&1)"
RC=$?
if [ "${RC}" -eq 0 ]; then
    echo "  PASS: check_lint.sh passes clean on real repo"
    PASS=$((PASS + 1))
else
    echo "  FAIL: check_lint.sh failed on real repo:"
    echo "${OUT}"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="
[ "${FAIL}" -eq 0 ]

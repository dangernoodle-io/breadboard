#!/bin/sh
# Guard: fail if a canonical state topic is posted via bb_event_post() outside bb_cache.
#
# All four state topics must flow through bb_cache (B1-356). Any direct call to
# bb_event_post() referencing one of these topics outside bb_cache or test/ is a
# regression that re-splits the builder.
#
# Guarded topics (macro name -> string literal):
#   BB_NET_HEALTH_TOPIC   -> "net.health"
#   BB_DIAG_BOOT_TOPIC    -> "diag.boot"
#   BB_UPDATE_CHECK_TOPIC -> "update.available"
#   BB_DISPLAY_INFO_TOPIC -> "health.display"
#
# Excluded paths (legitimate callers):
#   platform/espidf/bb_cache/   -- implementation (posts via entry->topic variable)
#   platform/host/bb_cache/     -- implementation (same)
#   components/bb_cache/        -- component headers
#   test/                       -- tests post directly by design

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "${REPO_ROOT}" || exit 1

TOPIC_TOKENS='BB_NET_HEALTH_TOPIC|BB_DIAG_BOOT_TOPIC|BB_UPDATE_CHECK_TOPIC|BB_DISPLAY_INFO_TOPIC|"net\.health"|"diag\.boot"|"update\.available"|"health\.display"'

VIOLATIONS="$(grep -rn "bb_event_post(" \
    --include="*.c" --include="*.h" \
    --exclude-dir=".pio" \
    --exclude-dir=".claude" \
    . \
  | grep -E "${TOPIC_TOKENS}" \
  | grep -v "^\./platform/espidf/bb_cache/" \
  | grep -v "^\./platform/host/bb_cache/" \
  | grep -v "^\./components/bb_cache/" \
  | grep -v "^\./test/")"

if [ -n "${VIOLATIONS}" ]; then
    echo "${VIOLATIONS}" | while IFS= read -r match; do
        file="${match%%:*}"
        rest="${match#*:}"
        lineno="${rest%%:*}"
        echo "ERROR: state-topic bb_event_post outside bb_cache: ${file}:${lineno}"
    done
    count="$(echo "${VIOLATIONS}" | wc -l | tr -d ' ')"
    echo "check_state_topic_post: ${count} violation(s) found — state topics must be posted through bb_cache" >&2
    exit 1
fi

echo "check_state_topic_post: OK"

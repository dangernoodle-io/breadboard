#pragma once

/**
 * @brief Wire-format identifier shared by serialization emit-vtable
 * implementations (e.g. bb_serialize_emit_t.format_id) so a future
 * (format, key, version) render-memo can key on it.
 */

typedef enum {
    BB_FORMAT_NONE = 0,  // recording/test emit, no wire format
    BB_FORMAT_JSON,      // PR-2 bb_serialize_json backend
} bb_format_t;

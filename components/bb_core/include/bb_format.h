#pragma once

/**
 * @brief Wire-format identifier shared by serialization emit-vtable
 * implementations (e.g. bb_serialize_emit_t.format_id) so a future
 * (format, key, version) render-memo can key on it, and by the
 * bb_serialize format-dispatch registry (bb_serialize_format_register() et
 * al, in bb_serialize) which keys a bb_registry lookup on bb_format_name().
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_FORMAT_NONE = 0,  // recording/test emit, no wire format
    BB_FORMAT_JSON,      // PR-2 bb_serialize_json backend
    BB_FORMAT__COUNT,    // sentinel -- not a real format; array/table sizing only
} bb_format_t;

// Maps a bb_format_t to its stable, lowercase registry-key name (e.g.
// "json"). Returns NULL for BB_FORMAT_NONE (no wire format, nothing to
// register/look up under) and for any out-of-range value.
const char *bb_format_name(bb_format_t fmt);

#ifdef __cplusplus
}
#endif

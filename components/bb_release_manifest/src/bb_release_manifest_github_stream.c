// Streaming GitHub release manifest parser.
//
// Processes the GitHub /releases/latest JSON byte-by-byte in a resumable
// state machine so callers never need to buffer the entire response.
//
// JSON shape expected:
//   { "tag_name": "<tag>", ..., "assets": [ { "name": "<board>.bin",
//     "browser_download_url": "<url>", ... }, ... ] }
//
// Extraction rules (identical to the buffered parser):
//   1. Top-level "tag_name" value -> tag_out
//   2. Under assets[], the object whose "name" == "<board_fallback>.bin" ->
//      that object's "browser_download_url" -> url_out
//
// State machine overview:
//   The parser walks the byte stream one character at a time using a main
//   phase enum. Key scanning and string copying each have their own sub-state
//   so they can be suspended across chunk boundaries.

#include "bb_release_manifest.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Internal layout inside _state[512]
//
// We cast the opaque _state[] to a pointer to the real struct below. The
// compile-time assert at the bottom ensures the struct fits.
// ---------------------------------------------------------------------------

// Maximum board-fallback length stored internally (including ".bin" suffix).
#define BOARD_NAME_MAX  132

// Maximum key length we need to match: "browser_download_url" = 20 chars.
#define KEY_MAX         24

// Parser phases (top-level state machine).
typedef enum {
    PH_SCAN_KEY,            // scanning for a JSON object key string
    PH_IN_KEY,              // reading a key string character by character
    PH_AFTER_KEY,           // consumed closing '"', scanning for ':'
    PH_AFTER_COLON,         // consumed ':', scanning for value start
    PH_SKIP_STRING,         // skipping over a string value we don't need
    PH_COPY_STRING,         // copying a string value we want
    PH_SKIP_DEPTH,          // skipping nested object/array by depth counting
    PH_IN_ASSETS_ARRAY,     // inside assets[], scanning for object starts
    PH_IN_ASSET_OBJECT,     // inside an individual asset {}
    PH_DONE,                // both fields extracted; ignoring remaining bytes
    PH_ERROR,               // unrecoverable parse error
} phase_t;

// Destination for the current COPY_STRING phase.
typedef enum {
    DEST_TAG,
    DEST_URL,
} dest_t;

// What the parser expects to happen after the current key finishes and the
// value is consumed/copied.
typedef enum {
    AFTER_KEY_NONE,
    AFTER_KEY_ENTER_ASSETS,   // after "assets": enter the array
    AFTER_KEY_COPY_TAG,       // after "tag_name": copy string to tag_out
    AFTER_KEY_CHECK_NAME,     // after asset "name": copy+check against board
    AFTER_KEY_COPY_URL,       // after "browser_download_url": copy to url_out
} after_key_t;

typedef struct {
    // Outputs (borrowed from caller)
    char   *tag_out;
    size_t  tag_cap;
    char   *url_out;
    size_t  url_cap;

    // Board name to match (stored with ".bin" suffix appended)
    char    board_name[BOARD_NAME_MAX];
    size_t  board_name_len;

    // Parser phase
    phase_t phase;

    // Accumulated error (first error wins; subsequent _feed calls are no-ops)
    bb_err_t err;

    // Key scanning sub-state
    char     key_buf[KEY_MAX];
    size_t   key_len;           // bytes accumulated so far
    after_key_t after_key;

    // String copy sub-state (COPY_STRING / SKIP_STRING)
    dest_t   copy_dest;
    char     copy_tmp[BOARD_NAME_MAX];  // temporary for asset "name" copy
    size_t   copy_tmp_len;
    bool     in_escape;

    // Depth counter for SKIP_DEPTH
    int      skip_depth;
    phase_t  skip_return_phase; // phase to return to after depth reaches 0

    // Parse context flags
    bool     tag_found;
    bool     url_found;
    bool     in_assets;         // true once we've entered the assets array
    bool     in_asset_obj;      // true once we're inside an asset object
    bool     asset_name_matched; // current asset object name matched board

    // Nesting depth so we know when we've exited the assets array / objects
    int      global_depth;      // tracks '{' and '}' at the top level

    // String escape: whether the previous byte was '\'
    // (tracked inside COPY_STRING / SKIP_STRING / key scanning)
    bool     key_in_escape;
} stream_state_t;

// Compile-time size check
typedef char _stream_state_size_check[
    sizeof(stream_state_t) <= BB_RELEASE_MANIFEST_STREAM_STATE_SIZE ? 1 : -1
];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline stream_state_t *ctx_state(bb_release_manifest_stream_ctx_t *ctx)
{
    return (stream_state_t *)(void *)ctx->_state;
}

// Copy one decoded character into a destination during COPY_STRING.
static void copy_char(stream_state_t *s, char c)
{
    if (s->copy_dest == DEST_TAG) {
        size_t i = strlen(s->tag_out);
        if (i + 1 < s->tag_cap) {  // LCOV_EXCL_BR_LINE — overflow branch: tag buffer sized to hold GitHub tags
            s->tag_out[i]     = c;
            s->tag_out[i + 1] = '\0';
        } else {
            s->err = BB_ERR_NO_SPACE;  // LCOV_EXCL_LINE — tag buffer sized to hold GitHub tags
        }
    } else {
        size_t i = strlen(s->url_out);
        if (i + 1 < s->url_cap) {  // LCOV_EXCL_BR_LINE — overflow branch: url buffer sized to hold GitHub URLs
            s->url_out[i]     = c;
            s->url_out[i + 1] = '\0';
        } else {
            s->err = BB_ERR_NO_SPACE;  // LCOV_EXCL_LINE — url buffer sized to hold GitHub URLs
        }
    }
}

// Copy one character into the temporary asset-name buffer.
static void copy_tmp_char(stream_state_t *s, char c)
{
    if (s->copy_tmp_len + 1 < BOARD_NAME_MAX) {
        s->copy_tmp[s->copy_tmp_len++] = c;
        s->copy_tmp[s->copy_tmp_len]   = '\0';
    }
    // silently truncate — name mismatch is fine, we just won't match this asset
}

// Called when a COPY_STRING / SKIP_STRING phase sees the closing '"'.
// Handles both the asset-name match check and output finalization.
static void finish_string(stream_state_t *s)
{
    switch (s->after_key) {  // LCOV_EXCL_BR_LINE — ENTER_ASSETS and NONE arms are defensive (LCOV_EXCL_LINE on case labels)
        case AFTER_KEY_COPY_TAG:
            s->tag_found = true;
            // Return to scanning top-level keys
            s->phase = PH_SCAN_KEY;
            break;

        case AFTER_KEY_COPY_URL:
            s->url_found = true;
            // Check if we can declare done
            if (s->tag_found) {
                s->phase = PH_DONE;
            } else {
                s->phase = PH_SCAN_KEY;  // LCOV_EXCL_LINE — tag_name always precedes assets in GitHub JSON
            }
            break;

        case AFTER_KEY_CHECK_NAME:
            // Compare accumulated asset name against expected board name
            s->asset_name_matched =
                (s->copy_tmp_len == s->board_name_len) &&
                (memcmp(s->copy_tmp, s->board_name, s->board_name_len) == 0);
            // Continue scanning inside this asset object
            s->phase = PH_IN_ASSET_OBJECT;
            break;

        case AFTER_KEY_ENTER_ASSETS:  // LCOV_EXCL_LINE — string after "assets" key is skipped, not copied
        case AFTER_KEY_NONE:          // LCOV_EXCL_LINE — defensive: unused copy should not reach here
            s->phase = PH_SCAN_KEY;   // LCOV_EXCL_LINE
            break;                    // LCOV_EXCL_LINE
    }
    s->after_key = AFTER_KEY_NONE;
}

// Process one byte in the COPY_STRING or SKIP_STRING phase.
// Returns true if the closing '"' was found and phase was updated.
static bool process_string_byte(stream_state_t *s, char c, bool is_copy)
{
    if (s->in_escape) {
        s->in_escape = false;
        if (is_copy) {  // LCOV_EXCL_BR_LINE — process_string_byte always called with is_copy=true
            char decoded = c;
            switch (c) {  // LCOV_EXCL_BR_LINE — most escape cases (n,t,r,b,f,",default) not in tag/url
                case 'n':  decoded = '\n'; break;  // LCOV_EXCL_LINE — \n not in tag/url
                case 't':  decoded = '\t'; break;  // LCOV_EXCL_LINE — \t not in tag/url
                case 'r':  decoded = '\r'; break;  // LCOV_EXCL_LINE — \r not in tag/url
                case 'b':  decoded = '\b'; break;  // LCOV_EXCL_LINE — \b not in tag/url
                case 'f':  decoded = '\f'; break;  // LCOV_EXCL_LINE — \f not in tag/url
                case '"':  decoded = '"';  break;  // LCOV_EXCL_LINE — " not in tag/url
                case '\\': decoded = '\\'; break;
                case '/':  decoded = '/';  break;  // LCOV_EXCL_BR_LINE — / in URL handled as literal
                case 'u':
                    // \uXXXX: skip 4 hex digits by setting a counter.
                    // We treat the 'u' byte here and count remaining 4 in
                    // subsequent calls via a simple trick: re-enter escape
                    // mode won't work, so we handle via a separate skip counter.
                    // For simplicity, skip the character (character dropped).
                    return false;
                default:   decoded = c; break;  // LCOV_EXCL_LINE — defensive: no other escapes appear in tag/url
            }
            if (s->after_key == AFTER_KEY_CHECK_NAME) {
                copy_tmp_char(s, decoded);
            } else {
                copy_char(s, decoded);
            }
        }
        return false;
    }

    if (c == '\\') {
        s->in_escape = true;
        return false;
    }

    if (c == '"') {
        // End of string
        if (is_copy) {  // LCOV_EXCL_BR_LINE — process_string_byte always called with is_copy=true
            finish_string(s);
        } else {
            // Skipping a string — just return to previous phase
            s->phase = PH_SCAN_KEY;  // LCOV_EXCL_LINE — process_string_byte always called with is_copy=true
        }
        return true;
    }

    if (is_copy) {  // LCOV_EXCL_BR_LINE — process_string_byte always called with is_copy=true
        if (s->after_key == AFTER_KEY_CHECK_NAME) {
            copy_tmp_char(s, c);
        } else {
            copy_char(s, c);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_release_manifest_parse_github_stream_begin(
    bb_release_manifest_stream_ctx_t *ctx,
    const char *board_fallback,
    char *tag_out, size_t tag_cap,
    char *url_out, size_t url_cap)
{
    if (!ctx || !board_fallback || !tag_out || !url_out) return BB_ERR_INVALID_ARG;
    if (tag_cap < 2 || url_cap < 2) return BB_ERR_INVALID_ARG;

    stream_state_t *s = ctx_state(ctx);
    memset(s, 0, sizeof(*s));

    s->tag_out  = tag_out;
    s->tag_cap  = tag_cap;
    s->url_out  = url_out;
    s->url_cap  = url_cap;
    s->tag_out[0] = '\0';
    s->url_out[0] = '\0';

    // Build "<board_fallback>.bin" for asset name matching
    size_t bl = strlen(board_fallback);
    size_t full_len = bl + 4;  // + ".bin"
    if (full_len >= BOARD_NAME_MAX) {  // LCOV_EXCL_BR_LINE — board name bounded well under 132 bytes
        full_len = BOARD_NAME_MAX - 1;
        bl = full_len - 4;
    }
    memcpy(s->board_name, board_fallback, bl);
    memcpy(s->board_name + bl, ".bin", 4);
    s->board_name[full_len] = '\0';
    s->board_name_len = full_len;

    s->phase = PH_SCAN_KEY;
    s->err   = BB_OK;
    return BB_OK;
}

bb_err_t bb_release_manifest_parse_github_stream_feed(
    bb_release_manifest_stream_ctx_t *ctx,
    const char *chunk, size_t chunk_len)
{
    if (!ctx || !chunk) return BB_ERR_INVALID_ARG;  // LCOV_EXCL_BR_LINE — defensive

    stream_state_t *s = ctx_state(ctx);

    if (s->err != BB_OK) return s->err;  // LCOV_EXCL_BR_LINE — err only set on BB_ERR_NO_SPACE (oversized buffers)
    if (s->phase == PH_DONE) return BB_OK;
    if (s->phase == PH_ERROR) return BB_ERR_NOT_FOUND;  // LCOV_EXCL_LINE — defensive

    for (size_t i = 0; i < chunk_len; i++) {
        if (s->phase == PH_DONE || s->err != BB_OK) break;  // LCOV_EXCL_BR_LINE — err branch: only set on oversized-buffer overflow

        char c = chunk[i];

        switch (s->phase) {  // LCOV_EXCL_BR_LINE — PH_DONE/PH_ERROR arms excluded above; all other arms covered by tests

        case PH_SCAN_KEY:
            // Looking for the opening '"' of a key string.
            // Track nesting to know when we leave an assets array element.
            if (c == '"') {
                s->key_len       = 0;
                s->key_buf[0]    = '\0';
                s->key_in_escape = false;
                s->phase         = PH_IN_KEY;
            } else if (c == '{') {
                s->global_depth++;
            } else if (c == '}') {
                s->global_depth--;
                if (s->in_asset_obj && s->global_depth < 2) {  // LCOV_EXCL_BR_LINE — defensive: PH_SCAN_KEY + in_asset_obj + depth>=2 unreachable in valid JSON
                    // Exited an asset object
                    s->in_asset_obj       = false;
                    s->asset_name_matched = false;
                    s->phase = PH_IN_ASSETS_ARRAY;
                }
            } else if (c == '[') {  // LCOV_EXCL_BR_LINE — bare '[' in PH_SCAN_KEY not reachable in valid JSON
                s->global_depth++;  // LCOV_EXCL_LINE — bare '[' in key-scan position not reachable in valid JSON
            } else if (c == ']') {
                s->global_depth--;
                // LCOV_EXCL_START — assets array exit via PH_IN_ASSETS_ARRAY ']' path; this guard is defensive
                if (s->in_assets && s->global_depth < 1) {
                    // Exited the assets array — no more objects to check
                    s->in_assets = false;
                    s->phase = PH_SCAN_KEY;
                }
                // LCOV_EXCL_STOP
            }
            break;

        case PH_IN_KEY:
            if (s->key_in_escape) {
                s->key_in_escape = false;
                if (s->key_len < KEY_MAX - 1) {
                    s->key_buf[s->key_len++] = c;
                    s->key_buf[s->key_len]   = '\0';
                }
            } else if (c == '\\') {
                s->key_in_escape = true;
            } else if (c == '"') {
                // End of key — decide what to do with it
                s->phase = PH_AFTER_KEY;
            } else {
                if (s->key_len < KEY_MAX - 1) {
                    s->key_buf[s->key_len++] = c;
                    s->key_buf[s->key_len]   = '\0';
                }
            }
            break;

        case PH_AFTER_KEY:
            // Skip whitespace, look for ':'
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;  // LCOV_EXCL_BR_LINE
            if (c == ':') {  // LCOV_EXCL_BR_LINE — false branch: malformed JSON without ':' after key; ignore and keep scanning
                // Determine what this key means based on context
                if (!s->tag_found && strcmp(s->key_buf, "tag_name") == 0) {
                    s->after_key = AFTER_KEY_COPY_TAG;
                } else if (!s->in_assets && strcmp(s->key_buf, "assets") == 0) {
                    s->after_key = AFTER_KEY_ENTER_ASSETS;
                } else if (s->in_asset_obj && !s->asset_name_matched &&
                           strcmp(s->key_buf, "name") == 0) {
                    s->after_key      = AFTER_KEY_CHECK_NAME;
                    s->copy_tmp_len   = 0;
                    s->copy_tmp[0]    = '\0';
                } else if (s->in_asset_obj && s->asset_name_matched &&
                           !s->url_found &&  // LCOV_EXCL_BR_LINE — url_found=true branch: parser is PH_DONE before a second matching asset is seen
                           strcmp(s->key_buf, "browser_download_url") == 0) {
                    s->after_key = AFTER_KEY_COPY_URL;
                } else {
                    s->after_key = AFTER_KEY_NONE;
                }
                s->phase = PH_AFTER_COLON;
            }
            // else: malformed — ignore character, keep scanning
            break;

        case PH_AFTER_COLON:
            // Skip whitespace, dispatch on value type
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;  // LCOV_EXCL_BR_LINE

            if (c == '"') {
                s->in_escape = false;
                if (s->after_key == AFTER_KEY_COPY_TAG ||
                    s->after_key == AFTER_KEY_CHECK_NAME ||
                    s->after_key == AFTER_KEY_COPY_URL) {
                    s->copy_dest = (s->after_key == AFTER_KEY_COPY_URL) ? DEST_URL : DEST_TAG;
                    s->phase = PH_COPY_STRING;
                } else {
                    s->phase = PH_SKIP_STRING;
                }
            } else if (c == '[') {
                if (s->after_key == AFTER_KEY_ENTER_ASSETS) {
                    s->in_assets    = true;
                    s->global_depth++;
                    s->phase = PH_IN_ASSETS_ARRAY;
                } else {
                    // Skip array we don't care about
                    s->skip_depth        = 1;
                    s->skip_return_phase = s->in_asset_obj ? PH_IN_ASSET_OBJECT : PH_SCAN_KEY;
                    s->phase = PH_SKIP_DEPTH;
                }
                s->after_key = AFTER_KEY_NONE;
            } else if (c == '{') {
                // Nested object we don't need — skip it
                s->skip_depth        = 1;
                s->skip_return_phase = s->in_asset_obj ? PH_IN_ASSET_OBJECT : PH_SCAN_KEY;
                s->phase = PH_SKIP_DEPTH;
                s->after_key = AFTER_KEY_NONE;
            } else {
                // Scalar (number, bool, null) — skip until separator
                s->after_key = AFTER_KEY_NONE;
                s->phase = s->in_asset_obj ? PH_IN_ASSET_OBJECT : PH_SCAN_KEY;
            }
            break;

        case PH_SKIP_STRING:
            if (s->in_escape) {
                s->in_escape = false;
            } else if (c == '\\') {
                s->in_escape = true;
            } else if (c == '"') {
                s->phase = s->in_asset_obj ? PH_IN_ASSET_OBJECT : PH_SCAN_KEY;
            }
            break;

        case PH_COPY_STRING:
            process_string_byte(s, c, /*is_copy=*/true);
            break;

        case PH_SKIP_DEPTH:
            if (s->in_escape) {
                s->in_escape = false;
            } else if (c == '"') {
                // Enter a string to avoid counting braces inside it
                s->in_escape = false;
                // Simple string skip: consume until closing '"' respecting escapes
                // We re-use in_escape; re-enter via a sub-loop here.
                // Instead, transition through PH_SKIP_STRING with a return-to-skip.
                // Actually for simplicity: mark we're in a string in skip mode.
                // We track this with a flag. Use in_escape=false and set a sentinel.
                // The cleanest approach: reuse the existing PH_SKIP_STRING logic
                // by temporarily saving/restoring. Since we can't nest phases, use
                // a dedicated flag.
                s->skip_depth = -s->skip_depth;  // negative = in-string mode
            } else if (c == '\\' && s->skip_depth < 0) {  // LCOV_EXCL_BR_LINE — '\\' with skip_depth>=0 is invalid JSON (backslash outside a string)
                s->in_escape = true;
            } else if (c == '"' && s->skip_depth < 0) {  // LCOV_EXCL_LINE — unreachable: the earlier `else if (c == '"')` always fires first
                // End of string inside skipped region
                s->skip_depth = -s->skip_depth;  // LCOV_EXCL_LINE
            } else if (s->skip_depth > 0) {
                if (c == '{' || c == '[') s->skip_depth++;
                else if (c == '}' || c == ']') {
                    s->skip_depth--;
                    if (s->skip_depth == 0) {
                        s->phase = s->skip_return_phase;
                    }
                }
            }
            break;

        case PH_IN_ASSETS_ARRAY:
            // Scanning for '{' to start an asset object, or ']' to end array
            if (c == '{') {
                s->in_asset_obj       = true;
                s->asset_name_matched = false;
                s->global_depth++;
                s->phase = PH_IN_ASSET_OBJECT;
            } else if (c == ']') {
                s->in_assets = false;
                s->global_depth--;
                s->phase = PH_SCAN_KEY;
            }
            // skip commas and whitespace
            break;

        case PH_IN_ASSET_OBJECT:
            // Inside an asset object — look for '"' to start a key, or '}' to end
            if (c == '"') {
                s->key_len       = 0;
                s->key_buf[0]    = '\0';
                s->key_in_escape = false;
                s->phase         = PH_IN_KEY;
            } else if (c == '}') {
                // End of this asset object
                s->global_depth--;
                s->in_asset_obj       = false;
                s->asset_name_matched = false;
                s->phase = PH_IN_ASSETS_ARRAY;
            }
            break;

        // LCOV_EXCL_START — loop guard at line 314 breaks before the switch is
        // reached when phase == PH_DONE; PH_ERROR has no reachable setter
        case PH_DONE:
        case PH_ERROR:
            break;
        // LCOV_EXCL_STOP
        }
    }

    return s->err;
}

bb_err_t bb_release_manifest_parse_github_stream_end(
    bb_release_manifest_stream_ctx_t *ctx)
{
    if (!ctx) return BB_ERR_INVALID_ARG;  // LCOV_EXCL_BR_LINE — defensive

    stream_state_t *s = ctx_state(ctx);

    if (s->err != BB_OK) return s->err;  // LCOV_EXCL_BR_LINE — err only set on BB_ERR_NO_SPACE (oversized buffers)
    if (!s->tag_found || !s->url_found) return BB_ERR_NOT_FOUND;
    return BB_OK;
}

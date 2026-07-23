// bb_serialize_meta_openapi — composes a JSON Schema (draft 2020-12 subset)
// fragment from a bb_serialize_desc_t + its bb_serialize_desc_meta_t
// companion. Bounded-buffer, no heap, all-or-nothing overflow -- the SAME
// idiom as components/bb_serialize_json/src/bb_serialize_json.c (every
// byte funnels through a single capacity-checked put(); overflow sets a
// sticky error and stops writing, never a partial fragment). Host-only
// artifact (see bb_serialize_meta.h banner) -- NOT wired into bb_openapi's
// route registry.

#include "bb_serialize_meta.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Bounded writer state + low-level put()/putc() -- every byte funnels
// through bb_oa_put() so a single capacity check governs the whole write.
// ---------------------------------------------------------------------------

typedef struct {
    char    *buf;
    size_t   cap;
    size_t   len;
    bb_err_t err;
} bb_oa_ctx_t;

static void bb_oa_put(bb_oa_ctx_t *ctx, const char *s, size_t n)
{
    if (ctx->err != BB_OK) return;
    if (ctx->len + n > ctx->cap) {
        ctx->err = BB_ERR_NO_SPACE;
        return;
    }
    memcpy(ctx->buf + ctx->len, s, n);
    ctx->len += n;
}

static void bb_oa_putc(bb_oa_ctx_t *ctx, char ch)
{
    bb_oa_put(ctx, &ch, 1);
}

static void bb_oa_puts(bb_oa_ctx_t *ctx, const char *s)
{
    bb_oa_put(ctx, s, strlen(s));
}

// Writes a JSON string literal (quotes + minimal escaping) -- `s` is
// trusted static metadata (keys, titles, descriptions), so this escapes
// only `"` and `\\`, sufficient for the hand-authored content this
// composer is fed.
static void bb_oa_put_qstr(bb_oa_ctx_t *ctx, const char *s)
{
    bb_oa_putc(ctx, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') bb_oa_putc(ctx, '\\');
        bb_oa_putc(ctx, *p);
    }
    bb_oa_putc(ctx, '"');
}

// ---------------------------------------------------------------------------
// Field lookup -- same idiom as bb_serialize_meta_validate.c's find_row().
// ---------------------------------------------------------------------------

static const bb_serialize_field_meta_t *bb_oa_find_row(const bb_serialize_field_meta_t *rows,
                                                         uint16_t n_rows, const char *key)
{
    for (uint16_t i = 0; i < n_rows; i++) {
        if (strcmp(rows[i].key, key) == 0) return &rows[i];
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Schema composition
// ---------------------------------------------------------------------------

static void bb_oa_write_type(bb_oa_ctx_t *ctx, bb_type_t type)
{
    switch (type) {  // LCOV_EXCL_BR_LINE -- exhaustive enum switch: the
                      // compiler-generated jump-table bounds-check branch is
                      // only taken for an out-of-range value, unreachable
                      // given bb_type_t's closed 0..7 domain (same class of
                      // defensive-only branch as the `default:` case below)
    case BB_TYPE_I64:
    case BB_TYPE_U64:  bb_oa_puts(ctx, "\"integer\""); break;
    case BB_TYPE_F64:  bb_oa_puts(ctx, "\"number\"");  break;
    case BB_TYPE_BOOL: bb_oa_puts(ctx, "\"boolean\""); break;
    case BB_TYPE_STR:
    case BB_TYPE_STR_N: bb_oa_puts(ctx, "\"string\""); break;
    case BB_TYPE_OBJ:  bb_oa_puts(ctx, "\"object\"");  break;
    case BB_TYPE_ARR:  bb_oa_puts(ctx, "\"array\"");   break;
    case BB_TYPE_REF:
        // A REF composes a sibling section inline at runtime (resolved by
        // bb_serialize_walk_ref()'s resolve callback); the sibling's own
        // descriptor isn't known statically here, so this composer can't
        // expand its properties -- documented as an opaque object, same as
        // the runtime shape (a REF always renders as a nested JSON object).
        bb_oa_puts(ctx, "\"object\"");
        break;
    default:            bb_oa_puts(ctx, "\"null\"");   break;  // LCOV_EXCL_LINE -- exhaustive enum, defensive
    }
}

static void bb_oa_write_docs(bb_oa_ctx_t *ctx, const bb_serialize_field_meta_t *row)
{
    if (!row) return;

    if (row->title) {
        bb_oa_puts(ctx, ",\"title\":");
        bb_oa_put_qstr(ctx, row->title);
    }
    if (row->description) {
        bb_oa_puts(ctx, ",\"description\":");
        bb_oa_put_qstr(ctx, row->description);
    }
    if (row->format) {
        bb_oa_puts(ctx, ",\"format\":");
        bb_oa_put_qstr(ctx, row->format);
    }
    if (row->examples) {
        bb_oa_puts(ctx, ",\"examples\":[");
        for (uint16_t i = 0; row->examples[i]; i++) {
            if (i) bb_oa_putc(ctx, ',');
            bb_oa_puts(ctx, row->examples[i]);  // pre-JSON-encoded literal
        }
        bb_oa_putc(ctx, ']');
    }
    if (row->enum_vals) {
        bb_oa_puts(ctx, ",\"enum\":[");
        for (uint16_t i = 0; row->enum_vals[i]; i++) {
            if (i) bb_oa_putc(ctx, ',');
            bb_oa_put_qstr(ctx, row->enum_vals[i]);
        }
        bb_oa_putc(ctx, ']');
    }
}

static void bb_oa_write_field_schema(bb_oa_ctx_t *ctx, const bb_serialize_field_t *f,
                                      const bb_serialize_field_meta_t *row, unsigned depth);

// Element schema for a BB_TYPE_ARR field -- STR shape or OBJ shape.
static void bb_oa_write_items(bb_oa_ctx_t *ctx, const bb_serialize_field_t *f,
                               const bb_serialize_field_meta_t *row, unsigned depth)
{
    bb_oa_puts(ctx, ",\"items\":{");
    if (f->elem_type == BB_TYPE_OBJ) {
        bb_oa_puts(ctx, "\"type\":\"object\"");
        if (depth < BB_SERIALIZE_MAX_DEPTH) {
            const bb_serialize_field_meta_t *child_rows = row ? row->children : NULL;
            uint16_t child_n_rows = row ? row->n_children : 0;
            bb_oa_puts(ctx, ",\"properties\":{");
            bool first = true;
            for (uint16_t i = 0; i < f->n_children; i++) {
                if (!first) bb_oa_putc(ctx, ',');
                first = false;
                bb_oa_put_qstr(ctx, f->children[i].key);
                bb_oa_putc(ctx, ':');
                bb_oa_write_field_schema(ctx, &f->children[i],
                                          bb_oa_find_row(child_rows, child_n_rows, f->children[i].key),
                                          depth + 1);
            }
            bb_oa_putc(ctx, '}');
        }
        bb_oa_puts(ctx, ",\"additionalProperties\":false");
    } else {
        bb_oa_puts(ctx, "\"type\":");
        bb_oa_write_type(ctx, f->elem_type);
    }
    bb_oa_putc(ctx, '}');
}

// Writes one field's full JSON Schema object: {"type":...,[constraints],[docs]}.
static void bb_oa_write_field_schema(bb_oa_ctx_t *ctx, const bb_serialize_field_t *f,
                                      const bb_serialize_field_meta_t *row, unsigned depth)
{
    bb_oa_putc(ctx, '{');
    bb_oa_puts(ctx, "\"type\":");
    bb_oa_write_type(ctx, f->type);

    if (row) {
        if (row->min_len) {
            bb_oa_puts(ctx, ",\"minLength\":");
            char num[8];
            int  n = snprintf(num, sizeof(num), "%u", (unsigned)row->min_len);
            bb_oa_put(ctx, num, (size_t)n);
        }
        if (row->has_min) {
            bb_oa_puts(ctx, ",\"minimum\":");
            char num[32];
            int  n = snprintf(num, sizeof(num), "%g", row->min);
            bb_oa_put(ctx, num, (size_t)n);
        }
        if (row->has_max) {
            bb_oa_puts(ctx, ",\"maximum\":");
            char num[32];
            int  n = snprintf(num, sizeof(num), "%g", row->max);
            bb_oa_put(ctx, num, (size_t)n);
        }
    }

    if (f->type == BB_TYPE_ARR) {
        bb_oa_write_items(ctx, f, row, depth);
    }

    if (f->type == BB_TYPE_OBJ && depth < BB_SERIALIZE_MAX_DEPTH) {
        const bb_serialize_field_meta_t *child_rows = row ? row->children : NULL;
        uint16_t child_n_rows = row ? row->n_children : 0;

        bb_oa_puts(ctx, ",\"properties\":{");
        bool first_prop = true;
        for (uint16_t i = 0; i < f->n_children; i++) {
            if (!first_prop) bb_oa_putc(ctx, ',');
            first_prop = false;
            const bb_serialize_field_meta_t *child_row =
                bb_oa_find_row(child_rows, child_n_rows, f->children[i].key);
            bb_oa_put_qstr(ctx, f->children[i].key);
            bb_oa_putc(ctx, ':');
            bb_oa_write_field_schema(ctx, &f->children[i], child_row, depth + 1);
        }
        bb_oa_putc(ctx, '}');

        bb_oa_puts(ctx, ",\"required\":[");
        bool first_req = true;
        for (uint16_t i = 0; i < f->n_children; i++) {
            const bb_serialize_field_meta_t *child_row =
                bb_oa_find_row(child_rows, child_n_rows, f->children[i].key);
            if (!child_row || !child_row->required) continue;
            if (!first_req) bb_oa_putc(ctx, ',');
            first_req = false;
            bb_oa_put_qstr(ctx, f->children[i].key);
        }
        bb_oa_putc(ctx, ']');
        bb_oa_puts(ctx, ",\"additionalProperties\":false");
    }

    bb_oa_write_docs(ctx, row);
    bb_oa_putc(ctx, '}');
}

bb_err_t bb_serialize_meta_openapi_schema(const bb_serialize_desc_t      *desc,
                                           const bb_serialize_desc_meta_t *meta,
                                           char *out, size_t out_size, size_t *out_len)
{
    if (out_size == 0) return BB_ERR_NO_SPACE;

    bb_oa_ctx_t ctx = { .buf = out, .cap = out_size - 1, .len = 0, .err = BB_OK };

    bb_oa_puts(&ctx, "{\"type\":\"object\",\"properties\":{");
    bool first = true;
    for (uint16_t i = 0; i < desc->n_fields; i++) {
        const bb_serialize_field_t      *f = &desc->fields[i];
        const bb_serialize_field_meta_t *row =
            meta ? bb_oa_find_row(meta->rows, meta->n_rows, f->key) : NULL;

        if (!first) bb_oa_putc(&ctx, ',');
        first = false;
        bb_oa_put_qstr(&ctx, f->key);
        bb_oa_putc(&ctx, ':');
        bb_oa_write_field_schema(&ctx, f, row, 0);
    }
    bb_oa_puts(&ctx, "},\"required\":[");

    bool first_req = true;
    for (uint16_t i = 0; i < desc->n_fields; i++) {
        const bb_serialize_field_t      *f = &desc->fields[i];
        const bb_serialize_field_meta_t *row =
            meta ? bb_oa_find_row(meta->rows, meta->n_rows, f->key) : NULL;
        if (!row || !row->required) continue;
        if (!first_req) bb_oa_putc(&ctx, ',');
        first_req = false;
        bb_oa_put_qstr(&ctx, f->key);
    }
    bb_oa_puts(&ctx, "],\"additionalProperties\":false}");

    if (ctx.err != BB_OK) {
        out[0] = '\0';
        if (out_len) *out_len = 0;
        return ctx.err;
    }

    out[ctx.len] = '\0';
    if (out_len) *out_len = ctx.len;
    return BB_OK;
}

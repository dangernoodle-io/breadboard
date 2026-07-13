// Host tests for the JSON scanner core (bb_serialize_json_scan_*) --
// a recording mock bb_serialize_json_ingest_t sink drives assertions over
// bounded and streaming (chunk-split) parses.

#include "unity.h"
#include "bb_serialize_json.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Recording ingest mock
// ---------------------------------------------------------------------------

typedef enum {
    OP_BEGIN_OBJ,
    OP_END_OBJ,
    OP_BEGIN_ARR,
    OP_END_ARR,
    OP_NUM,
    OP_BOOL,
    OP_NUL,
    OP_STR_CHUNK,
} rec_op_t;

typedef struct {
    rec_op_t    op;
    bool        has_key;
    char        key[32];
    char        val[64];   // num text, or str chunk content (truncated/copied)
    size_t      val_len;
    const char *val_ptr;   // raw pointer handed to the callback (span identity)
    bool        b;         // OP_BOOL value
    bool        is_final;  // OP_STR_CHUNK only
    bb_serialize_json_span_t provenance;  // OP_STR_CHUNK only -- span_provenance as received
} rec_t;

#define REC_MAX 128

static rec_t  s_rec[REC_MAX];
static size_t s_rec_n;

// Forward-declared: also clears the per-callback rejection state (defined
// below) so every test starts from a clean slate with one call, regardless
// of what a prior test in the same run left set.
static void reject_reset(void);

static void rec_reset(void)
{
    s_rec_n = 0;
    reject_reset();
}

static rec_t *rec_push(rec_op_t op, const char *key, size_t key_len)
{
    TEST_ASSERT_TRUE(s_rec_n < REC_MAX);
    rec_t *r = &s_rec[s_rec_n++];
    memset(r, 0, sizeof(*r));
    r->op = op;
    r->has_key = (key != NULL);
    if (key) {
        size_t n = key_len < sizeof(r->key) - 1 ? key_len : sizeof(r->key) - 1;
        memcpy(r->key, key, n);
        r->key[n] = '\0';
    }
    return r;
}

// Per-callback forced-rejection codes; BB_OK (default) means "don't reject".
// `s_reject_X_at` is a 1-based call index (0 disables) so a test can target
// e.g. the 2nd invocation of a callback rather than always the 1st.
static bb_err_t s_reject_begin_obj;
static int      s_reject_begin_obj_at;
static int      s_begin_obj_calls;
static bb_err_t s_reject_end_obj;
static bb_err_t s_reject_begin_arr;
static bb_err_t s_reject_end_arr;
static bb_err_t s_reject_value_num;
static bb_err_t s_reject_value_bool;
static bb_err_t s_reject_value_null;
static bb_err_t s_reject_value_str_chunk;
static int      s_reject_value_str_chunk_at;
static int      s_value_str_chunk_calls;

static void reject_reset(void)
{
    s_reject_begin_obj = BB_OK;
    s_reject_begin_obj_at = 0;
    s_begin_obj_calls = 0;
    s_reject_end_obj = BB_OK;
    s_reject_begin_arr = BB_OK;
    s_reject_end_arr = BB_OK;
    s_reject_value_num = BB_OK;
    s_reject_value_bool = BB_OK;
    s_reject_value_null = BB_OK;
    s_reject_value_str_chunk = BB_OK;
    s_reject_value_str_chunk_at = 0;
    s_value_str_chunk_calls = 0;
}

static bb_err_t mock_begin_obj(void *ctx, const char *key, size_t key_len)
{
    (void)ctx;
    rec_push(OP_BEGIN_OBJ, key, key_len);
    s_begin_obj_calls++;
    if (s_reject_begin_obj_at != 0) {
        return (s_begin_obj_calls == s_reject_begin_obj_at) ? BB_ERR_UNSUPPORTED : BB_OK;
    }
    return s_reject_begin_obj;
}

static bb_err_t mock_end_obj(void *ctx)
{
    (void)ctx;
    rec_push(OP_END_OBJ, NULL, 0);
    return s_reject_end_obj;
}

static bb_err_t mock_begin_arr(void *ctx, const char *key, size_t key_len)
{
    (void)ctx;
    rec_push(OP_BEGIN_ARR, key, key_len);
    return s_reject_begin_arr;
}

static bb_err_t mock_end_arr(void *ctx)
{
    (void)ctx;
    rec_push(OP_END_ARR, NULL, 0);
    return s_reject_end_arr;
}

static bb_err_t mock_value_num(void *ctx, const char *key, size_t key_len,
                                const char *num, size_t num_len)
{
    (void)ctx;
    rec_t *r = rec_push(OP_NUM, key, key_len);
    size_t n = num_len < sizeof(r->val) - 1 ? num_len : sizeof(r->val) - 1;
    if (num && n) memcpy(r->val, num, n);
    r->val[n] = '\0';
    r->val_len = num_len;
    return s_reject_value_num;
}

static bb_err_t mock_value_bool(void *ctx, const char *key, size_t key_len, bool v)
{
    (void)ctx;
    rec_t *r = rec_push(OP_BOOL, key, key_len);
    r->b = v;
    return s_reject_value_bool;
}

static bb_err_t mock_value_null(void *ctx, const char *key, size_t key_len)
{
    (void)ctx;
    rec_push(OP_NUL, key, key_len);
    return s_reject_value_null;
}

static bb_err_t mock_value_str_chunk(void *ctx, const char *key, size_t key_len,
                                      const char *chunk, size_t chunk_len, bool is_final,
                                      bb_serialize_json_span_t span_provenance)
{
    (void)ctx;
    rec_t *r = rec_push(OP_STR_CHUNK, key, key_len);
    size_t n = chunk_len < sizeof(r->val) - 1 ? chunk_len : sizeof(r->val) - 1;
    if (chunk && n) memcpy(r->val, chunk, n);
    r->val[n] = '\0';
    r->val_len = chunk_len;
    r->val_ptr = chunk;
    r->is_final = is_final;
    r->provenance = span_provenance;
    s_value_str_chunk_calls++;
    if (s_reject_value_str_chunk_at != 0) {
        return (s_value_str_chunk_calls == s_reject_value_str_chunk_at) ? BB_ERR_UNSUPPORTED : BB_OK;
    }
    return s_reject_value_str_chunk;
}

static const bb_serialize_json_ingest_t s_mock_sink = {
    .ctx = NULL,
    .begin_obj = mock_begin_obj,
    .end_obj = mock_end_obj,
    .begin_arr = mock_begin_arr,
    .end_arr = mock_end_arr,
    .value_num = mock_value_num,
    .value_bool = mock_value_bool,
    .value_null = mock_value_null,
    .value_str_chunk = mock_value_str_chunk,
};

static void assert_key(const char *expected, bool has_key, const char *actual)
{
    if (!expected) {
        TEST_ASSERT_FALSE(has_key);
    } else {
        TEST_ASSERT_TRUE(has_key);
        TEST_ASSERT_EQUAL_STRING(expected, actual);
    }
}

// ---------------------------------------------------------------------------
// Scalars -- bounded mode
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_bounded_string_empty(void)
{
    rec_reset();
    const char *doc = "\"\"";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_STR_CHUNK, s_rec[0].op);
    TEST_ASSERT_FALSE(s_rec[0].has_key);
    TEST_ASSERT_EQUAL_UINT(0, s_rec[0].val_len);
    TEST_ASSERT_TRUE(s_rec[0].is_final);
}

void test_bb_serialize_json_scan_bounded_string_ascii(void)
{
    rec_reset();
    const char *doc = "\"hello\"";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_STR_CHUNK, s_rec[0].op);
    TEST_ASSERT_EQUAL_STRING("hello", s_rec[0].val);
    TEST_ASSERT_TRUE(s_rec[0].is_final);
}

void test_bb_serialize_json_scan_bounded_string_span_into_caller_buffer(void)
{
    rec_reset();
    char doc[] = "\"hello\"";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL_PTR(doc + 1, s_rec[0].val_ptr);
    // Provenance: a direct slice of `doc` -- safe to record as (ptr, len)
    // beyond this callback, since `doc` outlives the call (bounded mode).
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_CALLER_STABLE, s_rec[0].provenance);
}

// Provenance regression for the "single trailing escape" collapse case
// (FINDING 1): a string that is exactly one escape immediately before the
// closing quote (e.g. "\n") collapses to ONE value_str_chunk call with
// is_final=true -- the SAME call-count/is_final shape as the escape-free
// direct-span case above -- yet its span comes from scan_string's on-stack
// `decoded[4]` scratch, not from `doc`. A sink that infers span safety from
// call-count/is_final alone (rather than the provenance value) would record
// a dangling pointer here. Assert BOTH that the provenance is
// SCANNER_SCRATCH AND that val_ptr does NOT land inside `doc`'s bytes.
void test_bb_serialize_json_scan_bounded_string_single_trailing_escape_is_scanner_scratch(void)
{
    rec_reset();
    char doc[] = "\"\\n\"";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_TRUE(s_rec[0].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_SCANNER_SCRATCH, s_rec[0].provenance);
    TEST_ASSERT_EQUAL_STRING("\n", s_rec[0].val);
    // val_ptr must NOT be a slice of doc -- it must lie entirely outside
    // doc's byte range (it points into scan_string's on-stack scratch).
    bool ptr_inside_doc = s_rec[0].val_ptr >= doc && s_rec[0].val_ptr < doc + sizeof(doc);
    TEST_ASSERT_FALSE(ptr_inside_doc);
}

// Same collapse case, but with the SAME shape from streaming mode via a
// single feed() call (bounded mode's collapse is a structural consequence
// of drive() running once -- see the file banner in bb_serialize_json_scan.c
// -- so this confirms the streaming path emits the same provenance signal
// on the same code path, not a bounded-mode-only special case). Escape
// decodes are ALWAYS SCANNER_SCRATCH regardless of mode -- unlike direct
// spans, there is no CALLER_STABLE/CALLER_FEED_SCOPED split here.
void test_bb_serialize_json_scan_streaming_string_single_trailing_escape_is_scanner_scratch(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    char doc[] = "\"\\t\"";
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, doc, strlen(doc)));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));

    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_TRUE(s_rec[0].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_SCANNER_SCRATCH, s_rec[0].provenance);
    bool ptr_inside_doc = s_rec[0].val_ptr >= doc && s_rec[0].val_ptr < doc + sizeof(doc);
    TEST_ASSERT_FALSE(ptr_inside_doc);
}

// Locks down FINDING A's whole point: the SAME document, scanned via the
// two different entry points, must yield DIFFERENT provenance for the SAME
// direct-span run -- CALLER_STABLE (bounded, buf outlives the call) vs
// CALLER_FEED_SCOPED (streaming, span dies when _feed() returns). A sink
// that only ever saw bounded mode and inferred "false/direct == durable"
// would be wrong the first time it's wired to a streaming source.
void test_bb_serialize_json_scan_provenance_bounded_vs_streaming_same_run_span(void)
{
    const char *doc = "\"hello\"";

    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_CALLER_STABLE, s_rec[0].provenance);

    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, doc, strlen(doc)));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED, s_rec[0].provenance);
}

// ---------------------------------------------------------------------------
// FINDING B -- alternating provenance within one mixed string. A URL
// containing "\/" (an escaped forward slash) shows this exact shape: a
// direct-span run, then decoded-escape scratch, then another direct-span
// run -- the point at which a token recorder must switch between
// reference-and-copy MID-STRING. Assert provenance across all three chunks
// AND that the direct-span chunks' val_ptr lands inside `doc` while the
// scratch chunk's does not, for BOTH bounded and streaming modes.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_bounded_alternating_provenance(void)
{
    rec_reset();
    char doc[] = "\"ab\\ncd\"";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(3, s_rec_n);

    TEST_ASSERT_EQUAL_STRING("ab", s_rec[0].val);
    TEST_ASSERT_FALSE(s_rec[0].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_CALLER_STABLE, s_rec[0].provenance);
    TEST_ASSERT_TRUE(s_rec[0].val_ptr >= doc && s_rec[0].val_ptr < doc + sizeof(doc));

    TEST_ASSERT_EQUAL_STRING("\n", s_rec[1].val);
    TEST_ASSERT_FALSE(s_rec[1].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_SCANNER_SCRATCH, s_rec[1].provenance);
    TEST_ASSERT_FALSE(s_rec[1].val_ptr >= doc && s_rec[1].val_ptr < doc + sizeof(doc));

    TEST_ASSERT_EQUAL_STRING("cd", s_rec[2].val);
    TEST_ASSERT_TRUE(s_rec[2].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_CALLER_STABLE, s_rec[2].provenance);
    TEST_ASSERT_TRUE(s_rec[2].val_ptr >= doc && s_rec[2].val_ptr < doc + sizeof(doc));
}

void test_bb_serialize_json_scan_streaming_alternating_provenance(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    char doc[] = "\"ab\\ncd\"";
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, doc, strlen(doc)));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));

    TEST_ASSERT_EQUAL(3, s_rec_n);

    TEST_ASSERT_EQUAL_STRING("ab", s_rec[0].val);
    TEST_ASSERT_FALSE(s_rec[0].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED, s_rec[0].provenance);
    TEST_ASSERT_TRUE(s_rec[0].val_ptr >= doc && s_rec[0].val_ptr < doc + sizeof(doc));

    TEST_ASSERT_EQUAL_STRING("\n", s_rec[1].val);
    TEST_ASSERT_FALSE(s_rec[1].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_SCANNER_SCRATCH, s_rec[1].provenance);
    TEST_ASSERT_FALSE(s_rec[1].val_ptr >= doc && s_rec[1].val_ptr < doc + sizeof(doc));

    TEST_ASSERT_EQUAL_STRING("cd", s_rec[2].val);
    TEST_ASSERT_TRUE(s_rec[2].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED, s_rec[2].provenance);
    TEST_ASSERT_TRUE(s_rec[2].val_ptr >= doc && s_rec[2].val_ptr < doc + sizeof(doc));
}

// STREAMING mode CAN emit a zero-length is_final=true chunk for a NON-EMPTY
// string: when a feed() boundary falls between the last content byte and
// the closing quote, scan_string's inner run-scan breaks immediately with
// run_len == 0 and still reports is_final=true (bb_serialize_json_scan.c,
// the `chunk[*i] == '"'` branch). This is the case the value_str_chunk
// CONTRACT in bb_serialize_json.h scopes to BOUNDED mode only -- pin the
// real streaming shape here so a future change can't silently drop it.
void test_bb_serialize_json_scan_streaming_split_before_closing_quote_emits_zero_length_final(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "{\"x\":\"ab", 8));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "\"}", 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));

    TEST_ASSERT_EQUAL(4, s_rec_n);  // begin_obj, str chunk, str final, end_obj

    TEST_ASSERT_EQUAL(OP_STR_CHUNK, s_rec[1].op);
    TEST_ASSERT_EQUAL_STRING("ab", s_rec[1].val);
    TEST_ASSERT_EQUAL(2, s_rec[1].val_len);
    TEST_ASSERT_FALSE(s_rec[1].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED, s_rec[1].provenance);

    TEST_ASSERT_EQUAL(OP_STR_CHUNK, s_rec[2].op);
    TEST_ASSERT_EQUAL(0, s_rec[2].val_len);
    TEST_ASSERT_TRUE(s_rec[2].is_final);
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED, s_rec[2].provenance);
}

static void assert_single_escape(const char *doc, const char *expected)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL_STRING(expected, s_rec[0].val);
    TEST_ASSERT_EQUAL_UINT(strlen(expected), s_rec[0].val_len);
    // Every single-escape-collapse case is scanner scratch, never a slice
    // of `doc` -- see test_..._single_trailing_escape_is_scanner_scratch for the
    // dedicated pointer-identity regression.
    TEST_ASSERT_EQUAL(BB_SERIALIZE_JSON_SPAN_SCANNER_SCRATCH, s_rec[0].provenance);
}

void test_bb_serialize_json_scan_bounded_string_escape_quote(void) { assert_single_escape("\"\\\"\"", "\""); }
void test_bb_serialize_json_scan_bounded_string_escape_backslash(void) { assert_single_escape("\"\\\\\"", "\\"); }
void test_bb_serialize_json_scan_bounded_string_escape_slash(void) { assert_single_escape("\"\\/\"", "/"); }
void test_bb_serialize_json_scan_bounded_string_escape_b(void) { assert_single_escape("\"\\b\"", "\b"); }
void test_bb_serialize_json_scan_bounded_string_escape_f(void) { assert_single_escape("\"\\f\"", "\f"); }
void test_bb_serialize_json_scan_bounded_string_escape_n(void) { assert_single_escape("\"\\n\"", "\n"); }
void test_bb_serialize_json_scan_bounded_string_escape_r(void) { assert_single_escape("\"\\r\"", "\r"); }
void test_bb_serialize_json_scan_bounded_string_escape_t(void) { assert_single_escape("\"\\t\"", "\t"); }

void test_bb_serialize_json_scan_bounded_string_escape_u_bmp(void)
{
    // é -> "é" (UTF-8: 0xC3 0xA9)
    assert_single_escape("\"\\u00e9\"", "\xC3\xA9");
}

void test_bb_serialize_json_scan_bounded_string_escape_u_surrogate_pair(void)
{
    // U+1F600 (grinning face) -> surrogate pair 😀 -> UTF-8 F0 9F 98 80
    assert_single_escape("\"\\ud83d\\ude00\"", "\xF0\x9F\x98\x80");
}

void test_bb_serialize_json_scan_bounded_int(void)
{
    rec_reset();
    const char *doc = "42";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_NUM, s_rec[0].op);
    TEST_ASSERT_EQUAL_STRING("42", s_rec[0].val);
}

void test_bb_serialize_json_scan_bounded_int_negative(void)
{
    rec_reset();
    const char *doc = "-17";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("-17", s_rec[0].val);
}

void test_bb_serialize_json_scan_bounded_float_frac(void)
{
    rec_reset();
    const char *doc = "3.5";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("3.5", s_rec[0].val);
}

void test_bb_serialize_json_scan_bounded_float_exp_lower_e(void)
{
    rec_reset();
    const char *doc = "1e3";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("1e3", s_rec[0].val);
}

void test_bb_serialize_json_scan_bounded_float_exp_upper_e(void)
{
    rec_reset();
    const char *doc = "1E3";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("1E3", s_rec[0].val);
}

void test_bb_serialize_json_scan_bounded_float_exp_plus(void)
{
    rec_reset();
    const char *doc = "1.5e+3";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("1.5e+3", s_rec[0].val);
}

void test_bb_serialize_json_scan_bounded_float_exp_minus(void)
{
    rec_reset();
    const char *doc = "1.5e-3";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("1.5e-3", s_rec[0].val);
}

void test_bb_serialize_json_scan_bounded_true(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("true", 4, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[0].op);
    TEST_ASSERT_TRUE(s_rec[0].b);
}

void test_bb_serialize_json_scan_bounded_false(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("false", 5, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[0].op);
    TEST_ASSERT_FALSE(s_rec[0].b);
}

void test_bb_serialize_json_scan_bounded_null(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("null", 4, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(OP_NUL, s_rec[0].op);
}

// ---------------------------------------------------------------------------
// Structural fixtures
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_bounded_object_and_array(void)
{
    rec_reset();
    const char *doc = "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{}}";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    size_t idx = 0;
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[idx].op);
    assert_key(NULL, s_rec[idx].has_key, s_rec[idx].key); idx++;
    TEST_ASSERT_EQUAL(OP_NUM, s_rec[idx].op);
    assert_key("a", s_rec[idx].has_key, s_rec[idx].key);
    TEST_ASSERT_EQUAL_STRING("1", s_rec[idx].val); idx++;
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[idx].op);
    assert_key("b", s_rec[idx].has_key, s_rec[idx].key); idx++;
    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[idx].op);
    TEST_ASSERT_FALSE(s_rec[idx].has_key);
    TEST_ASSERT_TRUE(s_rec[idx].b); idx++;
    TEST_ASSERT_EQUAL(OP_NUL, s_rec[idx].op);
    TEST_ASSERT_FALSE(s_rec[idx].has_key); idx++;
    TEST_ASSERT_EQUAL(OP_STR_CHUNK, s_rec[idx].op);
    TEST_ASSERT_EQUAL_STRING("x", s_rec[idx].val); idx++;
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[idx].op); idx++;
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[idx].op);
    assert_key("c", s_rec[idx].has_key, s_rec[idx].key); idx++;
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[idx].op); idx++;
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[idx].op); idx++;
    TEST_ASSERT_EQUAL(idx, s_rec_n);
}

void test_bb_serialize_json_scan_bounded_whitespace_tolerant(void)
{
    rec_reset();
    const char *doc = " \t\r\n {  \"a\"  :  1  ,  \"b\"  :  2  } \t\r\n ";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(4, s_rec_n);
}

// ---------------------------------------------------------------------------
// Streaming: split-across-chunk boundaries
// ---------------------------------------------------------------------------

static bb_err_t feed_split(const char *doc, size_t split_at)
{
    bb_serialize_json_scan_ctx_t ctx;
    size_t len = strlen(doc);
    TEST_ASSERT_TRUE(split_at <= len);

    bb_err_t rc = bb_serialize_json_scan_begin(&ctx, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    rc = bb_serialize_json_scan_feed(&ctx, doc, split_at);
    if (rc != BB_OK) return rc;
    rc = bb_serialize_json_scan_feed(&ctx, doc + split_at, len - split_at);
    if (rc != BB_OK) return rc;
    return bb_serialize_json_scan_end(&ctx);
}

void test_bb_serialize_json_scan_split_mid_key(void)
{
    rec_reset();
    const char *doc = "{\"hello\":1}";
    bb_err_t rc = feed_split(doc, 4);  // splits inside "hello"
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(OP_NUM, s_rec[1].op);
    assert_key("hello", s_rec[1].has_key, s_rec[1].key);
}

void test_bb_serialize_json_scan_split_mid_string_content(void)
{
    rec_reset();
    const char *doc = "\"hello world\"";
    // doc bytes: 0:", 1..11:"hello world", 12:"; split at 7 lands right
    // after "hello " (indices 1..6).
    bb_err_t rc = feed_split(doc, 7);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("hello ", s_rec[0].val);
    TEST_ASSERT_FALSE(s_rec[0].is_final);
    TEST_ASSERT_EQUAL_STRING("world", s_rec[1].val);
    TEST_ASSERT_TRUE(s_rec[1].is_final);
}

void test_bb_serialize_json_scan_split_on_escape_backslash(void)
{
    rec_reset();
    const char *doc = "\"a\\nb\"";
    // doc bytes: " a \ n b "  -> indices: 0:",1:a,2:\,3:n,4:b,5:"
    size_t split_at = 3;  // ends right after the backslash byte
    bb_err_t rc = feed_split(doc, split_at);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    // "a" run, decoded '\n', "b" run+final
    TEST_ASSERT_EQUAL(3, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("a", s_rec[0].val);
    TEST_ASSERT_EQUAL_STRING("\n", s_rec[1].val);
    TEST_ASSERT_EQUAL_STRING("b", s_rec[2].val);
    TEST_ASSERT_TRUE(s_rec[2].is_final);
}

void test_bb_serialize_json_scan_split_mid_u_escape(void)
{
    rec_reset();
    const char *doc = "\"\\u00e9\"";
    // doc bytes: " \ u 0 0 e 9 "  indices 0..7; split between the two hex-digit halves
    bb_err_t rc = feed_split(doc, 5);  // ends right after "00"
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9", s_rec[0].val);
}

void test_bb_serialize_json_scan_split_mid_surrogate_pair(void)
{
    rec_reset();
    const char *doc = "\"\\ud83d\\ude00\"";
    // split between the two \u escapes (right after the high surrogate)
    bb_err_t rc = feed_split(doc, 7);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("\xF0\x9F\x98\x80", s_rec[0].val);
}

void test_bb_serialize_json_scan_split_mid_number(void)
{
    rec_reset();
    const char *doc = "1234";
    bb_err_t rc = feed_split(doc, 2);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("1234", s_rec[0].val);
}

void test_bb_serialize_json_scan_split_mid_literal(void)
{
    rec_reset();
    const char *doc = "true";
    bb_err_t rc = feed_split(doc, 3);  // "tru" | "e"
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BOOL, s_rec[0].op);
    TEST_ASSERT_TRUE(s_rec[0].b);
}

// ---------------------------------------------------------------------------
// Malformed documents
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_malformed_literal_mismatch(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("trux", 4, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_number_leading_plus(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("+1", 2, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_number_leading_dot(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded(".5", 2, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_trailing_garbage(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("42 43", 5, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_unterminated_string_control_char(void)
{
    rec_reset();
    const char doc[] = { '"', 'a', 0x01, 'b', '"' };
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, sizeof(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_unbalanced_brackets(void)
{
    rec_reset();
    const char *doc = "{\"a\":1]";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_unpaired_high_surrogate(void)
{
    rec_reset();
    const char *doc = "\"\\ud83d\"";  // high surrogate, no low surrogate follows
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_unpaired_high_surrogate_not_followed_by_escape(void)
{
    rec_reset();
    const char *doc = "\"\\ud83dx\"";  // high surrogate followed by a plain char, not '\'
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_high_surrogate_followed_by_non_u_escape(void)
{
    rec_reset();
    const char *doc = "\"\\ud83d\\n\"";  // high surrogate then \n instead of \u
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_invalid_low_surrogate(void)
{
    rec_reset();
    const char *doc = "\"\\ud83d\\u0041\"";  // high surrogate + a non-low-surrogate \u
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_standalone_low_surrogate(void)
{
    rec_reset();
    const char *doc = "\"\\ude00\"";  // a low surrogate with no preceding high surrogate
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_bad_escape(void)
{
    rec_reset();
    const char *doc = "\"\\q\"";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_bad_hex_digit(void)
{
    rec_reset();
    const char *doc = "\"\\u00zz\"";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_bad_value_start(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("x", 1, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_bad_key_start(void)
{
    rec_reset();
    const char *doc = "{a:1}";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_missing_colon(void)
{
    rec_reset();
    const char *doc = "{\"a\" 1}";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_missing_comma_in_object(void)
{
    rec_reset();
    const char *doc = "{\"a\":1 \"b\":2}";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_missing_comma_in_array(void)
{
    rec_reset();
    const char *doc = "[1 2]";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_key_only_no_close_allowed(void)
{
    rec_reset();
    const char *doc = "{\"a\":1,}";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

// ---------------------------------------------------------------------------
// Truncated documents (scan_end() with pending state)
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_truncated_mid_container(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "{\"a\":1", 6));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_serialize_json_scan_end(&ctx));
}

void test_bb_serialize_json_scan_truncated_mid_string(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "\"abc", 4));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_serialize_json_scan_end(&ctx));
}

void test_bb_serialize_json_scan_truncated_mid_literal(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "tru", 3));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_serialize_json_scan_end(&ctx));
}

void test_bb_serialize_json_scan_truncated_mid_number(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "-", 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_serialize_json_scan_end(&ctx));
}

void test_bb_serialize_json_scan_truncated_mid_number_trailing_dot(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("1.", 2, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

void test_bb_serialize_json_scan_truncated_mid_number_trailing_exp(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("1e", 2, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

void test_bb_serialize_json_scan_truncated_mid_number_trailing_exp_sign(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("1e+", 3, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

void test_bb_serialize_json_scan_truncated_no_value_at_all(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "   ", 3));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_serialize_json_scan_end(&ctx));
}

void test_bb_serialize_json_scan_root_number_completes_at_end(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "42", 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));
    TEST_ASSERT_EQUAL(1, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("42", s_rec[0].val);
}

// ---------------------------------------------------------------------------
// Depth overflow
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_depth_overflow(void)
{
    rec_reset();
    const char *doc = "[[[[[[[[[1]]]]]]]]]";  // 9 nested arrays, cap is 8
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

void test_bb_serialize_json_scan_depth_at_cap_ok(void)
{
    rec_reset();
    const char *doc = "[[[[[[[[1]]]]]]]]";  // 8 nested arrays, exactly at cap
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// Sink rejection at every callback site
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_sink_rejects_begin_obj(void)
{
    rec_reset();
    reject_reset();
    s_reject_begin_obj = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("{\"a\":1}", 7, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);  // only begin_obj recorded, nothing after
}

void test_bb_serialize_json_scan_sink_rejects_end_obj(void)
{
    rec_reset();
    reject_reset();
    s_reject_end_obj = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("{\"a\":1}", 7, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(3, s_rec_n);  // begin_obj + value_num + end_obj (rejected, still recorded)
}

void test_bb_serialize_json_scan_sink_rejects_begin_arr(void)
{
    rec_reset();
    reject_reset();
    s_reject_begin_arr = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("[1]", 3, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
}

void test_bb_serialize_json_scan_sink_rejects_end_arr(void)
{
    rec_reset();
    reject_reset();
    s_reject_end_arr = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("[1]", 3, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(3, s_rec_n);  // begin_arr + value_num + end_arr (rejected, still recorded)
}

void test_bb_serialize_json_scan_sink_rejects_value_num(void)
{
    rec_reset();
    reject_reset();
    s_reject_value_num = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("[1,2]", 5, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);  // begin_arr + first value_num (rejected)
}

void test_bb_serialize_json_scan_sink_rejects_value_bool(void)
{
    rec_reset();
    reject_reset();
    s_reject_value_bool = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("[true,false]", 12, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);
}

void test_bb_serialize_json_scan_sink_rejects_value_null(void)
{
    rec_reset();
    reject_reset();
    s_reject_value_null = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("[null,1]", 8, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);
}

void test_bb_serialize_json_scan_sink_rejects_value_str_chunk(void)
{
    rec_reset();
    reject_reset();
    s_reject_value_str_chunk = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("[\"x\",1]", 7, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);  // begin_arr + rejected str chunk, no value_num after
}

// ---------------------------------------------------------------------------
// Scratch overflow
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_scratch_overflow_key(void)
{
    rec_reset();
    char doc[128];
    size_t n = 0;
    doc[n++] = '{';
    doc[n++] = '"';
    for (int i = 0; i < 100; i++) doc[n++] = 'k';  // key far longer than the 64-byte scratch
    doc[n++] = '"';
    doc[n++] = ':';
    doc[n++] = '1';
    doc[n++] = '}';
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, n, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

void test_bb_serialize_json_scan_scratch_overflow_number(void)
{
    rec_reset();
    char doc[128];
    size_t n = 0;
    for (int i = 0; i < 100; i++) doc[n++] = '9';  // number far longer than the 64-byte scratch
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, n, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: utf8_encode / hex_val ranges not hit by the fixtures
// above.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_bounded_string_escape_u_ascii_one_byte(void)
{
    assert_single_escape("\"\\u0041\"", "A");
}

void test_bb_serialize_json_scan_bounded_string_escape_u_three_byte(void)
{
    // U+4E2D (中) -> UTF-8 E4 B8 AD
    assert_single_escape("\"\\u4e2d\"", "\xE4\xB8\xAD");
}

void test_bb_serialize_json_scan_bounded_string_escape_u_uppercase_hex(void)
{
    // U+00C9 (É) -> UTF-8 C3 89; exercises the A-F hex_val branch
    assert_single_escape("\"\\u00C9\"", "\xC3\x89");
}

void test_bb_serialize_json_scan_malformed_low_surrogate_bad_hex_digit(void)
{
    rec_reset();
    const char *doc = "\"\\ud83d\\uzzzz\"";  // valid high surrogate, garbage low hex
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_low_surrogate_too_high(void)
{
    rec_reset();
    const char *doc = "\"\\ud83d\\uffff\"";  // valid high surrogate, low unit above 0xDFFF
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: the escape-decode "peek for closing quote" boundary
// when the peek itself falls off the end of the current chunk (as opposed
// to finding a non-quote byte).
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_split_right_after_escape_before_peek(void)
{
    rec_reset();
    const char *doc = "\"\\u0041\"";
    // doc bytes: 0:", 1:\, 2:u, 3:0, 4:0, 5:4, 6:1, 7:"; split at 7 ends
    // right after the 4th hex digit, so the quote-peek has nothing left in
    // this chunk to look at.
    bb_err_t rc = feed_split(doc, 7);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("A", s_rec[0].val);
    TEST_ASSERT_FALSE(s_rec[0].is_final);
    TEST_ASSERT_EQUAL_UINT(0, s_rec[1].val_len);
    TEST_ASSERT_TRUE(s_rec[1].is_final);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: sink rejection at every distinct emission site
// inside scan_string (final-via-escape, non-final-via-escape, run-flush-at-
// chunk-boundary, run-flush-before-escape) and inside dispatch_value_start
// nested under a keyed object member.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_sink_rejects_escape_final_chunk(void)
{
    rec_reset();
    reject_reset();
    s_reject_value_str_chunk = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("[\"\\n\"]", 6, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);  // begin_arr + rejected final decoded-escape chunk
}

void test_bb_serialize_json_scan_sink_rejects_escape_non_final_chunk(void)
{
    rec_reset();
    reject_reset();
    s_reject_value_str_chunk = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("[\"\\na\"]", 7, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);  // begin_arr + rejected non-final decoded-escape chunk
}

void test_bb_serialize_json_scan_sink_rejects_run_flush_at_chunk_boundary(void)
{
    rec_reset();
    reject_reset();
    s_reject_value_str_chunk = BB_ERR_UNSUPPORTED;

    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    bb_err_t rc = bb_serialize_json_scan_feed(&ctx, "\"hello", 6);  // mid-run, chunk ends
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);
}

void test_bb_serialize_json_scan_sink_rejects_run_flush_before_escape(void)
{
    rec_reset();
    reject_reset();
    s_reject_value_str_chunk = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("\"ab\\ncd\"", 8, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(1, s_rec_n);  // "ab" run flushed (and rejected) before the escape
}

void test_bb_serialize_json_scan_sink_rejects_nested_obj_value(void)
{
    rec_reset();
    reject_reset();
    s_reject_begin_obj_at = 2;  // reject only the inner (2nd) begin_obj call
    bb_err_t rc = bb_serialize_json_scan_bounded("{\"a\":{\"b\":1}}", 13, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);  // outer begin_obj (ok) + inner begin_obj (rejected)
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: depth overflow via nested OBJECTS (not just arrays),
// and scratch overflow landing on the very first byte of a value (dispatch
// time, not mid-scan_number).
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_depth_overflow_nested_objects(void)
{
    rec_reset();
    // 9 nested objects (each keyed "a"), cap is 8.
    char doc[256];
    size_t n = 0;
    for (int i = 0; i < 9; i++) {
        doc[n++] = '{'; doc[n++] = '"'; doc[n++] = 'a'; doc[n++] = '"'; doc[n++] = ':';
    }
    doc[n++] = '1';
    for (int i = 0; i < 9; i++) doc[n++] = '}';
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, n, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

void test_bb_serialize_json_scan_scratch_overflow_at_negative_number_start(void)
{
    rec_reset();
    char doc[128];
    size_t n = 0;
    doc[n++] = '{';
    doc[n++] = '"';
    for (int i = 0; i < 64; i++) doc[n++] = 'k';  // key fills the 64-byte scratch exactly
    doc[n++] = '"';
    doc[n++] = ':';
    doc[n++] = '-';
    doc[n++] = '1';
    doc[n++] = '}';
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, n, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

void test_bb_serialize_json_scan_scratch_overflow_at_digit_number_start(void)
{
    rec_reset();
    char doc[128];
    size_t n = 0;
    doc[n++] = '{';
    doc[n++] = '"';
    for (int i = 0; i < 64; i++) doc[n++] = 'k';  // key fills the 64-byte scratch exactly
    doc[n++] = '"';
    doc[n++] = ':';
    doc[n++] = '5';
    doc[n++] = '}';
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, n, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: grammar edge cases (trailing comma before an array
// close) and the *i>=len "need more data" branch at every remaining phase.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_malformed_array_trailing_comma_no_close_allowed(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("[1,]", 4, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_split_right_after_obj_open(void)
{
    rec_reset();
    const char *doc = "{\"a\":1}";
    bb_err_t rc = feed_split(doc, 1);  // chunk1 == "{" only
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

void test_bb_serialize_json_scan_split_right_after_key_before_colon(void)
{
    rec_reset();
    const char *doc = "{\"a\":1}";
    bb_err_t rc = feed_split(doc, 4);  // chunk1 == "{\"a\"" (key closed, colon pending)
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

void test_bb_serialize_json_scan_split_right_after_colon(void)
{
    rec_reset();
    const char *doc = "{\"a\":1}";
    bb_err_t rc = feed_split(doc, 5);  // chunk1 == "{\"a\":" (value pending)
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

void test_bb_serialize_json_scan_split_right_after_obj_value_before_close(void)
{
    rec_reset();
    const char *doc = "{\"a\":1,\"b\":2}";
    bb_err_t rc = feed_split(doc, 6);  // chunk1 == "{\"a\":1" (comma/close pending)
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(4, s_rec_n);
}

void test_bb_serialize_json_scan_split_right_after_arr_open(void)
{
    rec_reset();
    const char *doc = "[1]";
    bb_err_t rc = feed_split(doc, 1);  // chunk1 == "[" only
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

void test_bb_serialize_json_scan_split_right_after_arr_comma(void)
{
    rec_reset();
    const char *doc = "[1,2]";
    bb_err_t rc = feed_split(doc, 3);  // chunk1 == "[1," (comma consumed, element pending)
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(4, s_rec_n);
}

void test_bb_serialize_json_scan_split_right_after_arr_value_before_close(void)
{
    rec_reset();
    const char *doc = "[1,2]";
    bb_err_t rc = feed_split(doc, 2);  // chunk1 == "[1" (comma/close pending)
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(4, s_rec_n);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: the sticky-error short-circuit in _feed and _end.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_feed_after_error_is_sticky_noop(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_serialize_json_scan_feed(&ctx, "x", 1));
    size_t before = s_rec_n;
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_serialize_json_scan_feed(&ctx, "1", 1));
    TEST_ASSERT_EQUAL(before, s_rec_n);  // no further processing occurred
}

void test_bb_serialize_json_scan_end_after_feed_error_returns_sticky(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_serialize_json_scan_feed(&ctx, "x", 1));
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_serialize_json_scan_end(&ctx));
}

void test_bb_serialize_json_scan_end_root_number_sink_reject(void)
{
    rec_reset();
    reject_reset();
    s_reject_value_num = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("42", 2, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: hex_val's A-F range with an out-of-range uppercase
// letter, and the low-surrogate "too high" (non-error) combination -- a
// \uXXXX code unit >= 0xDC00 but > 0xDFFF is NOT a low surrogate at all, so
// it must fall through to a normal (valid) encode, not an error.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_malformed_bad_hex_digit_uppercase_out_of_range(void)
{
    rec_reset();
    const char *doc = "\"\\u00ZZ\"";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_bounded_string_escape_u_above_surrogate_range(void)
{
    // U+E000 is >= 0xDC00 but > 0xDFFF -- not a surrogate at all, must
    // encode normally rather than tripping the "standalone low surrogate"
    // check.
    assert_single_escape("\"\\ue000\"", "\xEE\x80\x80");
}

// Note: scan_number's grammar-transition arms (int/frac/exp digits, '.',
// 'e'/'E', sign) all funnel through ONE shared scratch_append() choke point
// (see the file banner / scan_number's comment) -- test_bb_serialize_
// json_scan_scratch_overflow_number above already exercises that single
// call site's overflow branch; per-transition overflow variants would be
// redundant (same code, different phase label).

// ---------------------------------------------------------------------------
// Branch-coverage fill: hex_val's "below '0'" branch (first sub-condition
// false on both the digit and A-F checks).
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_malformed_hex_digit_below_zero_char(void)
{
    rec_reset();
    const char *doc = "\"\\u00..\"";  // '.' (0x2E) is below '0' -- fails every hex_val range
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: number-grammar "wrong continuation byte" guards --
// these are distinct from the scratch-overflow tests above, which always
// feed a VALID next byte (and so never take the "malformed" arm of the
// guard). Genuinely wrong bytes are needed to hit the other side.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_malformed_number_minus_not_followed_by_digit(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("-x", 2, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_number_dot_not_followed_by_digit(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("1.x", 3, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_number_exp_not_sign_or_digit(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("1ex", 3, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

void test_bb_serialize_json_scan_malformed_number_exp_sign_not_followed_by_digit(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("1e+x", 4, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, rc);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: numbers whose frac-digit / exp-digit run terminates
// via a real in-buffer structural byte (comma/close) rather than end of
// input -- exercises the "not e/E" and "not another digit" fallthrough arms
// that a bounded single-number document (which always terminates via EOF)
// never reaches.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_bounded_numbers_terminated_in_buffer(void)
{
    rec_reset();
    // 1.55 / 1e23 additionally exercise the FRAC_DIGITS / EXP_DIGITS
    // "another digit continues the run" arm (a single-digit frac/exp, as in
    // 1.5 or 1e2, never revisits that arm before hitting the terminator).
    const char *doc = "[1,1.5,1e2,1.55,1e23,1.5e3]";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(8, s_rec_n);  // begin_arr + 6 numbers + end_arr
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: sink rejection on an EMPTY object/array's end_obj/
// end_arr, called from the *_KEY_OR_CLOSE / *_ELEM_OR_CLOSE close path
// (distinct call site from the non-empty *_AFTER_VALUE close path already
// covered by test_bb_serialize_json_scan_sink_rejects_end_obj/end_arr).
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_sink_rejects_end_obj_on_empty_object(void)
{
    rec_reset();
    reject_reset();
    s_reject_end_obj = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("{}", 2, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);  // begin_obj + rejected end_obj
}

void test_bb_serialize_json_scan_sink_rejects_end_arr_on_empty_array(void)
{
    rec_reset();
    reject_reset();
    s_reject_end_arr = BB_ERR_UNSUPPORTED;
    bb_err_t rc = bb_serialize_json_scan_bounded("[]", 2, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);  // begin_arr + rejected end_arr
}

// A plain "[]" parsed to completion with a NON-rejecting sink -- the
// success-path counterpart to sink_rejects_end_arr_on_empty_array above.
// That test only exercises close_container()'s rc != BB_OK arm; this one
// exercises the rc == BB_OK arm on the same call site.
void test_bb_serialize_json_scan_bounded_empty_array(void)
{
    rec_reset();
    bb_err_t rc = bb_serialize_json_scan_bounded("[]", 2, &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL(2, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_ARR, s_rec[0].op);
    TEST_ASSERT_EQUAL(OP_END_ARR, s_rec[1].op);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: the "skip_ws then check *i>=len" boundary inside
// each of drive()'s post-structural-character phases. The boundary can only
// be reached if the phase is actually re-entered with bytes still pending
// in the current chunk (a structural char landing exactly at chunk end
// never re-enters the phase's own body -- the outer `while (*i < len)`
// guard already stops first) -- so each case below feeds a whitespace-only
// chunk right after the structural transition, guaranteeing skip_ws is what
// consumes the last byte.
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_split_ws_boundary_obj_key_or_close(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "{", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, " ", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "\"a\":1}", 6));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

void test_bb_serialize_json_scan_split_ws_boundary_obj_colon(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "{\"a\"", 4));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, " ", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, ":1}", 3));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

void test_bb_serialize_json_scan_split_ws_boundary_obj_value(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "{\"a\":", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, " ", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "1}", 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

void test_bb_serialize_json_scan_split_ws_boundary_obj_after_value(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "{\"a\":1", 6));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, " ", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "}", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

void test_bb_serialize_json_scan_split_ws_boundary_arr_elem(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "[", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, " ", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "1]", 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

void test_bb_serialize_json_scan_split_ws_boundary_arr_after_value(void)
{
    rec_reset();
    bb_serialize_json_scan_ctx_t ctx;
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_begin(&ctx, &s_mock_sink));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "[1", 2));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, " ", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_feed(&ctx, "]", 1));
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_json_scan_end(&ctx));
    TEST_ASSERT_EQUAL(3, s_rec_n);
}

// ---------------------------------------------------------------------------
// Branch-coverage fill: FRAC_DIGITS -> EXP_START transition on uppercase 'E'
// (the lowercase 'e' case is already covered by float_exp_lower_e /
// bounded_numbers_terminated_in_buffer).
// ---------------------------------------------------------------------------

void test_bb_serialize_json_scan_bounded_float_frac_then_exp_upper_e(void)
{
    rec_reset();
    const char *doc = "1.5E3";
    bb_err_t rc = bb_serialize_json_scan_bounded(doc, strlen(doc), &s_mock_sink);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("1.5E3", s_rec[0].val);
}

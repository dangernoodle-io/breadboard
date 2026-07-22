// Host tests for bb_storage_http_delete_wire (DELETE /api/diag/storage
// emit, object-wrapped {"deleted":[...],"key":...} shape). Renders the
// top-level bb_storage_http_delete_wire_desc via bb_serialize_json_render()
// (the same one-shot entry point storage_delete_handler ultimately drives
// through bb_http_serialize_stream()) and asserts the resulting JSON string
// exactly, byte for byte.

#include "unity.h"

#include "../../components/bb_diag_http/bb_storage_http_delete_wire_priv.h"

#include "bb_serialize_json.h"

#include <stdio.h>
#include <string.h>

#define RENDER_BUF_BYTES 1024

// Renders `names[0..count)` (via the same fill() helper the production
// handler uses) as the top-level {"deleted":[...],"key":...} object and
// returns the NUL-terminated JSON string in `out_buf` (caller-owned,
// RENDER_BUF_BYTES capacity).
static void delete_render(const char names[][BB_STORAGE_HTTP_DELETE_NS_LEN], size_t count,
                           const char *key, bool has_key, char *out_buf)
{
    bb_storage_http_delete_wire_t snap;
    bb_storage_http_delete_wire_fill(&snap, names, count, key, has_key);

    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_storage_http_delete_wire_desc, &snap,
                                            out_buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// 1. Zero namespaces, no key -> exactly {"deleted":[]}.
// ---------------------------------------------------------------------------

void test_bb_storage_http_delete_wire_expected_json_zero_no_key(void)
{
    char buf[RENDER_BUF_BYTES];
    delete_render(NULL, 0, NULL, false, buf);
    TEST_ASSERT_EQUAL_STRING("{\"deleted\":[]}", buf);
}

// ---------------------------------------------------------------------------
// 2. One namespace, no key.
// ---------------------------------------------------------------------------

void test_bb_storage_http_delete_wire_expected_json_one_ns_no_key(void)
{
    char names[1][BB_STORAGE_HTTP_DELETE_NS_LEN] = { "bb_mqtt" };

    char buf[RENDER_BUF_BYTES];
    delete_render(names, 1, NULL, false, buf);
    TEST_ASSERT_EQUAL_STRING("{\"deleted\":[\"bb_mqtt\"]}", buf);
}

// ---------------------------------------------------------------------------
// 3. One namespace + key.
// ---------------------------------------------------------------------------

void test_bb_storage_http_delete_wire_expected_json_ns_and_key(void)
{
    char names[1][BB_STORAGE_HTTP_DELETE_NS_LEN] = { "nvs" };

    char buf[RENDER_BUF_BYTES];
    delete_render(names, 1, "foo", true, buf);
    TEST_ASSERT_EQUAL_STRING("{\"deleted\":[\"nvs\"],\"key\":\"foo\"}", buf);
}

// ---------------------------------------------------------------------------
// 4. Multiple namespaces, no key.
// ---------------------------------------------------------------------------

void test_bb_storage_http_delete_wire_expected_json_multiple_ns(void)
{
    char names[3][BB_STORAGE_HTTP_DELETE_NS_LEN] = { "bb_mqtt", "bb_udp", "bb_tcp" };

    char buf[RENDER_BUF_BYTES];
    delete_render(names, 3, NULL, false, buf);
    TEST_ASSERT_EQUAL_STRING("{\"deleted\":[\"bb_mqtt\",\"bb_udp\",\"bb_tcp\"]}", buf);
}

// ---------------------------------------------------------------------------
// 5. Escaped namespace name (quote + backslash) -- defensive.
// ---------------------------------------------------------------------------

void test_bb_storage_http_delete_wire_expected_json_escaped_ns(void)
{
    char names[1][BB_STORAGE_HTTP_DELETE_NS_LEN];
    memset(names, 0, sizeof(names));
    strncpy(names[0], "a\"b\\c", sizeof(names[0]) - 1);

    char buf[RENDER_BUF_BYTES];
    delete_render(names, 1, NULL, false, buf);
    TEST_ASSERT_EQUAL_STRING("{\"deleted\":[\"a\\\"b\\\\c\"]}", buf);
}

// ---------------------------------------------------------------------------
// 6. Max-cap (BB_STORAGE_HTTP_DELETE_NS_MAX identical namespace names).
// ---------------------------------------------------------------------------

void test_bb_storage_http_delete_wire_expected_json_max_cap(void)
{
    char names[BB_STORAGE_HTTP_DELETE_NS_MAX][BB_STORAGE_HTTP_DELETE_NS_LEN];
    for (int i = 0; i < BB_STORAGE_HTTP_DELETE_NS_MAX; i++) {
        strncpy(names[i], "ns", sizeof(names[i]) - 1);
        names[i][sizeof(names[i]) - 1] = '\0';
    }

    char buf[RENDER_BUF_BYTES];
    delete_render(names, BB_STORAGE_HTTP_DELETE_NS_MAX, NULL, false, buf);

    char expected[RENDER_BUF_BYTES];
    size_t off = 0;
    off += (size_t)snprintf(expected + off, sizeof(expected) - off, "{\"deleted\":[");
    for (int i = 0; i < BB_STORAGE_HTTP_DELETE_NS_MAX; i++) {
        if (i > 0) expected[off++] = ',';
        off += (size_t)snprintf(expected + off, sizeof(expected) - off, "\"ns\"");
    }
    off += (size_t)snprintf(expected + off, sizeof(expected) - off, "]}");

    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

// ---------------------------------------------------------------------------
// 7. Present-predicate: "key" is omitted when has_key is false even when a
// non-NULL (but unused) key buffer is passed.
// ---------------------------------------------------------------------------

void test_bb_storage_http_delete_wire_key_omitted_when_absent(void)
{
    char buf[RENDER_BUF_BYTES];
    delete_render(NULL, 0, "ignored", false, buf);
    TEST_ASSERT_EQUAL_STRING("{\"deleted\":[]}", buf);
    TEST_ASSERT_NULL(strstr(buf, "key"));
}

// ---------------------------------------------------------------------------
// 8. Present-predicate: "key" is included when has_key is true.
// ---------------------------------------------------------------------------

void test_bb_storage_http_delete_wire_key_included_when_present(void)
{
    char buf[RENDER_BUF_BYTES];
    delete_render(NULL, 0, "bar", true, buf);
    TEST_ASSERT_EQUAL_STRING("{\"deleted\":[],\"key\":\"bar\"}", buf);
}

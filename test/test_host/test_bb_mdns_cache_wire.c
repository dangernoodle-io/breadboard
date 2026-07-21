// Host unit tests for bb_mdns_cache_entry_wire_desc/_fill (B1-1115 PR-3).
// Proves the EXACT rendered JSON shape of the new wire descriptor -- this
// pins the NEW shape (documented in bb_mdns_cache_wire_priv.h), not parity
// with the legacy entry_serialize() emitter; see that header's shape-change
// comment for the old-vs-new side by side.

#include "unity.h"

#include "../../components/bb_mdns_cache/bb_mdns_cache_wire_priv.h"
#include "bb_serialize_json.h"
#include "bb_str.h"

#include <stddef.h>
#include <string.h>

static void render_eq(const bb_serialize_desc_t *d, const void *snap, const char *golden)
{
    char   buf[512];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_serialize_json_render(d, snap, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_STRING(golden, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(golden), n);
}

// A representative CONSUMER entry struct -- identity fields at the same
// leading layout as bb_mdns_cache_entry_t (BB_MDNS_CACHE_ASSERT_IDENTITY_LAYOUT
// below turns a layout drift into a build error), plus two TXT-captured
// fields. Mirrors the shape a real bb_mdns_cache_config_t.entry_size/
// txt_fields consumer would declare.
typedef struct {
    char     hostname[BB_MDNS_CACHE_HOSTNAME_MAX];
    char     ip4[BB_MDNS_CACHE_IP4_MAX];
    uint16_t port;
    char     board[16];
    char     model[16];
} test_mdns_consumer_entry_t;

BB_MDNS_CACHE_ASSERT_IDENTITY_LAYOUT(test_mdns_consumer_entry_t);

static const bb_mdns_txt_field_t s_txt_fields[] = {
    { .txt_key = "board", .dest_offset = offsetof(test_mdns_consumer_entry_t, board),
      .dest_len = sizeof(((test_mdns_consumer_entry_t *)0)->board) },
    { .txt_key = "model", .dest_offset = offsetof(test_mdns_consumer_entry_t, model),
      .dest_len = sizeof(((test_mdns_consumer_entry_t *)0)->model) },
};

// ---------------------------------------------------------------------------
// No TXT descriptor -- identity-only peer, "txt" renders as an empty array.
// ---------------------------------------------------------------------------

void test_bb_mdns_cache_wire_render_no_txt(void)
{
    bb_mdns_cache_entry_t entry = {0};
    bb_strlcpy(entry.hostname, "peer.local", sizeof(entry.hostname));
    bb_strlcpy(entry.ip4, "10.0.0.5", sizeof(entry.ip4));
    entry.port = 8080;

    bb_mdns_cache_entry_wire_t wire;
    size_t dropped = 99;  // poisoned -- must come back 0, not left untouched
    bb_mdns_cache_entry_wire_fill(&wire, &entry, sizeof(entry), NULL, 0, &dropped);

    TEST_ASSERT_EQUAL_STRING("peer.local", wire.hostname);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", wire.ip4);
    TEST_ASSERT_EQUAL_INT64(8080, wire.port);
    TEST_ASSERT_EQUAL_UINT(0, wire.txt.count);
    TEST_ASSERT_EQUAL_UINT(0, dropped);

    render_eq(&bb_mdns_cache_entry_wire_desc, &wire,
        "{\"hostname\":\"peer.local\",\"ip4\":\"10.0.0.5\",\"port\":8080,\"txt\":[]}");
}

// txt_fields non-NULL but txt_count == 0 -- same empty-array outcome as NULL.
// out_dropped == NULL is also exercised here -- must not crash.
void test_bb_mdns_cache_wire_render_txt_fields_zero_count_is_empty(void)
{
    bb_mdns_cache_entry_t entry = {0};
    bb_strlcpy(entry.hostname, "peer.local", sizeof(entry.hostname));
    bb_strlcpy(entry.ip4, "10.0.0.5", sizeof(entry.ip4));
    entry.port = 1;

    bb_mdns_cache_entry_wire_t wire;
    bb_mdns_cache_entry_wire_fill(&wire, &entry, sizeof(entry), s_txt_fields, 0, NULL);

    TEST_ASSERT_EQUAL_UINT(0, wire.txt.count);
}

// txt_fields == NULL with a nonzero txt_count that exceeds the cap -- no
// fields were ever eligible to walk, so out_dropped must stay 0, not report
// a bogus cap-overflow count derived from txt_count alone.
void test_bb_mdns_cache_wire_fill_null_fields_high_count_no_drop(void)
{
    bb_mdns_cache_entry_t entry = {0};
    bb_strlcpy(entry.hostname, "peer.local", sizeof(entry.hostname));

    bb_mdns_cache_entry_wire_t wire;
    size_t dropped = 99;  // poisoned -- must come back 0
    bb_mdns_cache_entry_wire_fill(&wire, &entry, sizeof(entry), NULL,
                                   BB_MDNS_CACHE_WIRE_TXT_MAX + 5, &dropped);

    TEST_ASSERT_EQUAL_UINT(0, wire.txt.count);
    TEST_ASSERT_EQUAL_UINT(0, dropped);
}

// ---------------------------------------------------------------------------
// TXT descriptor configured -- "txt" renders as a populated array of
// {"key","value"} rows, one per captured field, in descriptor table order.
// ---------------------------------------------------------------------------

void test_bb_mdns_cache_wire_render_with_txt(void)
{
    test_mdns_consumer_entry_t entry = {0};
    bb_strlcpy(entry.hostname, "miner1.local", sizeof(entry.hostname));
    bb_strlcpy(entry.ip4, "192.168.1.50", sizeof(entry.ip4));
    entry.port = 4028;
    bb_strlcpy(entry.board, "s19", sizeof(entry.board));
    bb_strlcpy(entry.model, "bitmain", sizeof(entry.model));

    bb_mdns_cache_entry_wire_t wire;
    size_t dropped = 99;
    bb_mdns_cache_entry_wire_fill(&wire, &entry, sizeof(entry), s_txt_fields, 2, &dropped);

    TEST_ASSERT_EQUAL_UINT(2, wire.txt.count);
    TEST_ASSERT_EQUAL_STRING("board", wire.txt_items[0].key);
    TEST_ASSERT_EQUAL_STRING("s19", wire.txt_items[0].value);
    TEST_ASSERT_EQUAL_STRING("model", wire.txt_items[1].key);
    TEST_ASSERT_EQUAL_STRING("bitmain", wire.txt_items[1].value);
    TEST_ASSERT_EQUAL_UINT(0, dropped);

    render_eq(&bb_mdns_cache_entry_wire_desc, &wire,
        "{\"hostname\":\"miner1.local\",\"ip4\":\"192.168.1.50\",\"port\":4028,"
        "\"txt\":[{\"key\":\"board\",\"value\":\"s19\"},{\"key\":\"model\",\"value\":\"bitmain\"}]}");
}

// A field whose txt_key is NULL is skipped -- never counted, never read,
// and never counted as "dropped" (out_dropped only counts entries skipped
// for lack of room past BB_MDNS_CACHE_WIRE_TXT_MAX, not this kind of skip).
void test_bb_mdns_cache_wire_fill_field_null_key_skipped(void)
{
    static const bb_mdns_txt_field_t fields[] = {
        { .txt_key = NULL, .dest_offset = offsetof(test_mdns_consumer_entry_t, board),
          .dest_len = sizeof(((test_mdns_consumer_entry_t *)0)->board) },
        { .txt_key = "model", .dest_offset = offsetof(test_mdns_consumer_entry_t, model),
          .dest_len = sizeof(((test_mdns_consumer_entry_t *)0)->model) },
    };

    test_mdns_consumer_entry_t entry = {0};
    bb_strlcpy(entry.model, "bitmain", sizeof(entry.model));

    bb_mdns_cache_entry_wire_t wire;
    size_t dropped = 99;  // poisoned -- must come back 0, not 1
    bb_mdns_cache_entry_wire_fill(&wire, &entry, sizeof(entry), fields, 2, &dropped);

    TEST_ASSERT_EQUAL_UINT(1, wire.txt.count);
    TEST_ASSERT_EQUAL_STRING("model", wire.txt_items[0].key);
    TEST_ASSERT_EQUAL_UINT(0, dropped);
}

// A field whose [dest_offset, dest_offset+dest_len) range exceeds
// entry_size is skipped -- defensive bounds check, mirrors
// bb_mdns_cache_txt_serialize()'s own guard.
void test_bb_mdns_cache_wire_fill_field_out_of_bounds_skipped(void)
{
    static const bb_mdns_txt_field_t fields[] = {
        { .txt_key = "board", .dest_offset = offsetof(test_mdns_consumer_entry_t, board),
          .dest_len = sizeof(((test_mdns_consumer_entry_t *)0)->board) },
    };

    test_mdns_consumer_entry_t entry = {0};
    bb_strlcpy(entry.board, "s19", sizeof(entry.board));

    bb_mdns_cache_entry_wire_t wire;
    // entry_size deliberately truncated to just past identity -- the
    // "board" field's range now falls outside it.
    bb_mdns_cache_entry_wire_fill(&wire, &entry, sizeof(bb_mdns_cache_entry_t), fields, 1, NULL);

    TEST_ASSERT_EQUAL_UINT(0, wire.txt.count);
}

// A field's value read is bounded by min(dest_len, sizeof(row->value)), not
// just sizeof(row->value) -- proves the value copy never scans past the
// field's own declared dest_len even when a shorter dest_len than the wire
// row's own buffer is configured.
void test_bb_mdns_cache_wire_fill_value_bounded_by_dest_len(void)
{
    typedef struct {
        char     hostname[BB_MDNS_CACHE_HOSTNAME_MAX];
        char     ip4[BB_MDNS_CACHE_IP4_MAX];
        uint16_t port;
        char     board[16];  // holds "s19-longer-than-dest-len"-ish content
    } narrow_dest_entry_t;
    BB_MDNS_CACHE_ASSERT_IDENTITY_LAYOUT(narrow_dest_entry_t);

    static const bb_mdns_txt_field_t fields[] = {
        // dest_len (4, i.e. 3 chars + NUL) is deliberately narrower than
        // both the source buffer (16) and the wire row's value[32].
        { .txt_key = "board", .dest_offset = offsetof(narrow_dest_entry_t, board), .dest_len = 4 },
    };

    narrow_dest_entry_t entry = {0};
    bb_strlcpy(entry.board, "s19", sizeof(entry.board));

    bb_mdns_cache_entry_wire_t wire;
    bb_mdns_cache_entry_wire_fill(&wire, &entry, sizeof(entry), fields, 1, NULL);

    TEST_ASSERT_EQUAL_UINT(1, wire.txt.count);
    TEST_ASSERT_EQUAL_STRING("s19", wire.txt_items[0].value);
}

// More configured fields than BB_MDNS_CACHE_WIRE_TXT_MAX -- capture stops
// at the cap, never overruns dst->txt_items, and out_dropped signals the
// truncation (the fill itself stays silent/log-free by design -- see
// bb_mdns_cache_wire_priv.h's doc comment).
void test_bb_mdns_cache_wire_fill_caps_at_wire_txt_max(void)
{
    typedef struct {
        char     hostname[BB_MDNS_CACHE_HOSTNAME_MAX];
        char     ip4[BB_MDNS_CACHE_IP4_MAX];
        uint16_t port;
        char     f[BB_MDNS_CACHE_WIRE_TXT_MAX + 1][8];
    } big_entry_t;
    BB_MDNS_CACHE_ASSERT_IDENTITY_LAYOUT(big_entry_t);

    static bb_mdns_txt_field_t fields[BB_MDNS_CACHE_WIRE_TXT_MAX + 1];
    static const char *keys[BB_MDNS_CACHE_WIRE_TXT_MAX + 1] = {
        "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7", "k8",
    };
    big_entry_t entry = {0};
    for (size_t i = 0; i < BB_MDNS_CACHE_WIRE_TXT_MAX + 1; i++) {
        fields[i] = (bb_mdns_txt_field_t){
            .txt_key = keys[i],
            .dest_offset = offsetof(big_entry_t, f) + i * sizeof(entry.f[0]),
            .dest_len = sizeof(entry.f[0]),
        };
        bb_strlcpy(entry.f[i], keys[i], sizeof(entry.f[i]));
    }

    bb_mdns_cache_entry_wire_t wire;
    size_t dropped = 0;
    bb_mdns_cache_entry_wire_fill(&wire, &entry, sizeof(entry), fields,
                                   BB_MDNS_CACHE_WIRE_TXT_MAX + 1, &dropped);

    TEST_ASSERT_EQUAL_UINT(BB_MDNS_CACHE_WIRE_TXT_MAX, wire.txt.count);
    TEST_ASSERT_EQUAL_STRING("k0", wire.txt_items[0].key);
    TEST_ASSERT_EQUAL_STRING("k7", wire.txt_items[BB_MDNS_CACHE_WIRE_TXT_MAX - 1].key);
    TEST_ASSERT_EQUAL_UINT(1, dropped);
}

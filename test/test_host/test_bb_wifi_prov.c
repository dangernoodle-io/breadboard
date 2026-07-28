#include "unity.h"
#include "bb_wifi_prov.h"
#include <string.h>

void test_prov_parse_empty_body(void)
{
    char ssid[32], pass[64], hostname[33];
    bb_wifi_prov_parse_result_t result =
        bb_wifi_prov_parse_body("", 0, ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_EMPTY_BODY, result);
}

void test_prov_parse_missing_ssid(void)
{
    char ssid[32], pass[64], hostname[33];
    const char *body = "pass=secret";
    bb_wifi_prov_parse_result_t result = bb_wifi_prov_parse_body(
        body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_SSID_REQUIRED, result);
}

void test_prov_parse_ssid_only(void)
{
    char ssid[32], pass[64], hostname[33];
    const char *body = "ssid=test-net";
    bb_wifi_prov_parse_result_t result = bb_wifi_prov_parse_body(
        body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("test-net", ssid);
    TEST_ASSERT_EQUAL_STRING("", pass);
    TEST_ASSERT_EQUAL_STRING("", hostname);
}

void test_prov_parse_ssid_and_pass(void)
{
    char ssid[32], pass[64], hostname[33];
    const char *body = "ssid=test-net&pass=hunter2";
    bb_wifi_prov_parse_result_t result = bb_wifi_prov_parse_body(
        body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("test-net", ssid);
    TEST_ASSERT_EQUAL_STRING("hunter2", pass);
    TEST_ASSERT_EQUAL_STRING("", hostname);
}

void test_prov_parse_urlencoded_special(void)
{
    char ssid[32], pass[64], hostname[33];
    const char *body = "ssid=my%20net&pass=a%26b";
    bb_wifi_prov_parse_result_t result = bb_wifi_prov_parse_body(
        body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("my net", ssid);
    TEST_ASSERT_EQUAL_STRING("a&b", pass);
    TEST_ASSERT_EQUAL_STRING("", hostname);
}

void test_prov_parse_hostname_absent(void)
{
    char ssid[32], pass[64], hostname[33];
    const char *body = "ssid=test-net&pass=hunter2";
    bb_wifi_prov_parse_result_t result = bb_wifi_prov_parse_body(
        body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("", hostname);
}

void test_prov_parse_hostname_empty(void)
{
    char ssid[32], pass[64], hostname[33];
    const char *body = "ssid=test-net&pass=hunter2&hostname=";
    bb_wifi_prov_parse_result_t result = bb_wifi_prov_parse_body(
        body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("", hostname);
}

void test_prov_parse_hostname_present(void)
{
    char ssid[32], pass[64], hostname[33];
    const char *body = "ssid=test-net&pass=hunter2&hostname=my-board";
    bb_wifi_prov_parse_result_t result = bb_wifi_prov_parse_body(
        body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("test-net", ssid);
    TEST_ASSERT_EQUAL_STRING("hunter2", pass);
    TEST_ASSERT_EQUAL_STRING("my-board", hostname);
}

void test_prov_parse_hostname_urlencoded(void)
{
    char ssid[32], pass[64], hostname[33];
    const char *body = "ssid=test-net&hostname=my%20board";
    bb_wifi_prov_parse_result_t result = bb_wifi_prov_parse_body(
        body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_OK, result);
    TEST_ASSERT_EQUAL_STRING("my board", hostname);
}

void test_prov_parse_hostname_missing_ssid_still_required(void)
{
    char ssid[32], pass[64], hostname[33];
    const char *body = "hostname=my-board";
    bb_wifi_prov_parse_result_t result = bb_wifi_prov_parse_body(
        body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass), hostname, sizeof(hostname));
    TEST_ASSERT_EQUAL(BB_WIFI_PROV_PARSE_SSID_REQUIRED, result);
}

// ---------------------------------------------------------------------------
// bb_wifi_prov_html_escape
// ---------------------------------------------------------------------------

void test_prov_html_escape_plain_text_unchanged(void)
{
    char out[64];
    size_t n = bb_wifi_prov_html_escape("my-network", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("my-network", out);
    TEST_ASSERT_EQUAL(strlen("my-network"), n);
}

void test_prov_html_escape_all_special_chars(void)
{
    char out[64];
    size_t n = bb_wifi_prov_html_escape("<a>&\"'", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("&lt;a&gt;&amp;&quot;&#39;", out);
    TEST_ASSERT_EQUAL(strlen(out), n);
}

void test_prov_html_escape_null_src_is_empty(void)
{
    char out[8] = "stale";
    size_t n = bb_wifi_prov_html_escape(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(0, n);
}

void test_prov_html_escape_null_out_is_noop(void)
{
    size_t n = bb_wifi_prov_html_escape("<script>", NULL, 16);
    TEST_ASSERT_EQUAL(0, n);
}

void test_prov_html_escape_zero_out_size_is_noop(void)
{
    char out[8];
    size_t n = bb_wifi_prov_html_escape("<script>", out, 0);
    TEST_ASSERT_EQUAL(0, n);
}

void test_prov_html_escape_truncates_without_partial_entity(void)
{
    // out fits "ab" (2) + NUL (1) = 3; the trailing '&' entity ("&amp;",
    // 5 bytes) can't fit, so it must be dropped whole, never truncated
    // mid-entity ("&am" would corrupt the markup).
    char out[4];
    size_t n = bb_wifi_prov_html_escape("ab&cd", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("ab", out);
    TEST_ASSERT_EQUAL(2, n);
}

void test_prov_html_escape_truncates_plain_char_at_boundary(void)
{
    // out has room for exactly "ab" + NUL (3 bytes); the trailing 'c' is a
    // plain (non-entity) char that can't fit -- exercises the plain-char
    // truncation guard (distinct from the entity-truncation guard above,
    // which never reaches this branch since it breaks before ever trying
    // the plain-char path).
    char out[3];
    size_t n = bb_wifi_prov_html_escape("abc", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("ab", out);
    TEST_ASSERT_EQUAL(2, n);
}

// ---------------------------------------------------------------------------
// bb_wifi_prov_render_saved_page
// ---------------------------------------------------------------------------

void test_prov_render_saved_page_names_network(void)
{
    char out[BB_WIFI_PROV_SAVED_PAGE_MAX];
    size_t n = bb_wifi_prov_render_saved_page(out, sizeof(out), "my-net", "");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "my-net"));
    TEST_ASSERT_NOT_NULL(strstr(out, "about to disappear"));
}

void test_prov_render_saved_page_no_hostname_omits_mdns_lines(void)
{
    char out[BB_WIFI_PROV_SAVED_PAGE_MAX];
    bb_wifi_prov_render_saved_page(out, sizeof(out), "my-net", "");
    TEST_ASSERT_NULL(strstr(out, ".local"));
    TEST_ASSERT_NULL(strstr(out, "Hostname set"));
}

void test_prov_render_saved_page_includes_hostname_and_mdns_name(void)
{
    char out[BB_WIFI_PROV_SAVED_PAGE_MAX];
    bb_wifi_prov_render_saved_page(out, sizeof(out), "my-net", "my-board");
    TEST_ASSERT_NOT_NULL(strstr(out, "Hostname set"));
    TEST_ASSERT_NOT_NULL(strstr(out, "my-board.local"));
}

void test_prov_render_saved_page_escapes_attacker_controlled_ssid(void)
{
    char out[BB_WIFI_PROV_SAVED_PAGE_MAX];
    bb_wifi_prov_render_saved_page(out, sizeof(out), "<script>alert(1)</script>", "");
    TEST_ASSERT_NULL(strstr(out, "<script>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "&lt;script&gt;"));
}

void test_prov_render_saved_page_escapes_attacker_controlled_hostname(void)
{
    char out[BB_WIFI_PROV_SAVED_PAGE_MAX];
    bb_wifi_prov_render_saved_page(out, sizeof(out), "my-net", "\"><script>x</script>");
    TEST_ASSERT_NULL(strstr(out, "\"><script>"));
    TEST_ASSERT_NOT_NULL(strstr(out, "&quot;&gt;&lt;script&gt;"));
}

void test_prov_render_saved_page_null_out_is_noop(void)
{
    size_t n = bb_wifi_prov_render_saved_page(NULL, 16, "my-net", "");
    TEST_ASSERT_EQUAL(0, n);
}

void test_prov_render_saved_page_zero_out_size_is_noop(void)
{
    char out[8];
    size_t n = bb_wifi_prov_render_saved_page(out, 0, "my-net", "");
    TEST_ASSERT_EQUAL(0, n);
}

void test_prov_render_saved_page_null_hostname_omits_mdns_lines(void)
{
    // A NULL hostname (distinct from "") must be treated the same as
    // "no hostname" -- exercises has_hostname's null-check branch.
    char out[BB_WIFI_PROV_SAVED_PAGE_MAX];
    size_t n = bb_wifi_prov_render_saved_page(out, sizeof(out), "my-net", NULL);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NULL(strstr(out, ".local"));
    TEST_ASSERT_NULL(strstr(out, "Hostname set"));
}

void test_prov_render_saved_page_small_out_size_truncates(void)
{
    // A too-small (but non-zero) out_size forces the final snprintf's
    // "would-have-needed" length past out_size -- exercises
    // clamp_snprintf_result's truncation branch, and confirms the return
    // is clamped to what actually landed (out_size - 1), NUL-terminated.
    char out[20];
    size_t n = bb_wifi_prov_render_saved_page(out, sizeof(out), "my-net", "my-board");
    TEST_ASSERT_EQUAL(sizeof(out) - 1, n);
    TEST_ASSERT_EQUAL('\0', out[sizeof(out) - 1]);
}

// LOW review fix: a pathological worst-case hostname (32 bytes, every byte
// escaping to the longest entity, "&quot;") must never split an
// already-escaped entity across host_block's own internal truncation --
// host_block is sized from BB_WIFI_PROV_HTML_ESCAPED_MAX precisely so this
// never regresses back to a flat, too-small magic constant.
void test_prov_render_saved_page_worst_case_hostname_no_split_entity(void)
{
    char worst_hostname[33];
    memset(worst_hostname, '"', sizeof(worst_hostname) - 1);
    worst_hostname[sizeof(worst_hostname) - 1] = '\0';

    char out[BB_WIFI_PROV_SAVED_PAGE_MAX];
    size_t n = bb_wifi_prov_render_saved_page(out, sizeof(out), "my-net", worst_hostname);
    TEST_ASSERT_TRUE(n > 0);

    // Every emitted "&quot" must be immediately followed by its closing ';'
    // -- a split entity would leave a bare "&quot" with no ';' right after.
    const char *p = out;
    while ((p = strstr(p, "&quot")) != NULL) {
        TEST_ASSERT_EQUAL(';', p[5]);
        p += 5;
    }
}

// ----------------------------------------------------------------------
// bb_wifi_prov_saved_page_hostname
// ----------------------------------------------------------------------

void test_prov_saved_page_hostname_returns_hostname_when_saved(void)
{
    TEST_ASSERT_EQUAL_STRING("my-board", bb_wifi_prov_saved_page_hostname("my-board", true));
}

void test_prov_saved_page_hostname_returns_empty_when_not_saved(void)
{
    TEST_ASSERT_EQUAL_STRING("", bb_wifi_prov_saved_page_hostname("my-board", false));
}

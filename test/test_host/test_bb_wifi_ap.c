#include "unity.h"
#include "bb_wifi_ap.h"
#include <string.h>

// ---------------------------------------------------------------------------
// bb_wifi_ap_build_ssid
// ---------------------------------------------------------------------------

void test_bb_wifi_ap_build_ssid_formats_prefix_and_mac(void)
{
    const uint8_t mac[6] = { 0x00, 0x11, 0x22, 0x33, 0xAB, 0xCD };
    char out[32];
    bb_err_t err = bb_wifi_ap_build_ssid("BB-", mac, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("BB-ABCD", out);
}

void test_bb_wifi_ap_build_ssid_custom_prefix(void)
{
    const uint8_t mac[6] = { 0, 0, 0, 0, 0x01, 0x02 };
    char out[32];
    bb_err_t err = bb_wifi_ap_build_ssid("TaipanMiner-", mac, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("TaipanMiner-0102", out);
}

void test_bb_wifi_ap_build_ssid_null_prefix_invalid(void)
{
    const uint8_t mac[6] = { 0 };
    char out[32];
    bb_err_t err = bb_wifi_ap_build_ssid(NULL, mac, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_ap_build_ssid_null_mac_invalid(void)
{
    char out[32];
    bb_err_t err = bb_wifi_ap_build_ssid("BB-", NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_ap_build_ssid_null_out_invalid(void)
{
    const uint8_t mac[6] = { 0 };
    bb_err_t err = bb_wifi_ap_build_ssid("BB-", mac, NULL, 32);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_ap_build_ssid_zero_out_size_invalid(void)
{
    const uint8_t mac[6] = { 0 };
    char out[32];
    bb_err_t err = bb_wifi_ap_build_ssid("BB-", mac, out, 0);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_wifi_ap_build_ssid_out_size_too_small_invalid(void)
{
    const uint8_t mac[6] = { 0, 0, 0, 0, 0xAB, 0xCD };
    char out[4];  // "BB-A" fits, but not the full "BB-ABCD\0"
    bb_err_t err = bb_wifi_ap_build_ssid("BB-", mac, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

// ---------------------------------------------------------------------------
// bb_wifi_ap_normalize_prefix / bb_wifi_ap_normalize_password
// ---------------------------------------------------------------------------

void test_bb_wifi_ap_normalize_prefix_copies_value(void)
{
    char out[16];
    bb_wifi_ap_normalize_prefix("TaipanMiner-", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("TaipanMiner-", out);
}

void test_bb_wifi_ap_normalize_prefix_null_clears(void)
{
    char out[16] = "stale";
    bb_wifi_ap_normalize_prefix(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_bb_wifi_ap_normalize_prefix_null_out_noop(void)
{
    // Must not crash.
    bb_wifi_ap_normalize_prefix("BB-", NULL, 16);
}

void test_bb_wifi_ap_normalize_prefix_zero_size_noop(void)
{
    char out[16] = "stale";
    bb_wifi_ap_normalize_prefix("BB-", out, 0);
    TEST_ASSERT_EQUAL_STRING("stale", out);  // untouched
}

void test_bb_wifi_ap_normalize_password_copies_value(void)
{
    char out[64];
    bb_wifi_ap_normalize_password("s3cret!!", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("s3cret!!", out);
}

void test_bb_wifi_ap_normalize_password_null_defaults(void)
{
    char out[64];
    bb_wifi_ap_normalize_password(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("breadboard", out);
}

void test_bb_wifi_ap_normalize_password_null_out_noop(void)
{
    bb_wifi_ap_normalize_password("x", NULL, 64);
}

void test_bb_wifi_ap_normalize_password_zero_size_noop(void)
{
    char out[64] = "stale";
    bb_wifi_ap_normalize_password("x", out, 0);
    TEST_ASSERT_EQUAL_STRING("stale", out);
}

// ---------------------------------------------------------------------------
// bb_wifi_ap_dns_build_response
// ---------------------------------------------------------------------------

// A minimal 12-byte DNS header + 1-byte-label question ("a" -> root),
// QTYPE=A, QCLASS=IN. Good enough to exercise the response-building logic
// without a full-blown DNS message parser (the function only copies the
// question section through verbatim).
static void fill_min_query(uint8_t *q, int *out_len)
{
    memset(q, 0, 12);
    q[0] = 0xAB; q[1] = 0xCD;  // transaction id
    q[2] = 0x01; q[3] = 0x00;  // flags: standard query, recursion desired
    q[5] = 0x01;               // QDCOUNT = 1
    // Question: 1-byte label "a", then 0-length terminator, QTYPE=A, QCLASS=IN
    q[12] = 0x01; q[13] = 'a'; q[14] = 0x00;
    q[15] = 0x00; q[16] = 0x01;  // QTYPE A
    q[17] = 0x00; q[18] = 0x01;  // QCLASS IN
    *out_len = 19;
}

void test_bb_wifi_ap_dns_build_response_valid_query(void)
{
    uint8_t req[64];
    int req_len = 0;
    fill_min_query(req, &req_len);

    const uint8_t answer_ip[4] = { 192, 168, 4, 1 };
    uint8_t out[128];
    int resp_len = bb_wifi_ap_dns_build_response(req, req_len, answer_ip, out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(req_len + 16, resp_len);
    // Header flags: QR=1, AA=1
    TEST_ASSERT_EQUAL_HEX8(0x81, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x80, out[3]);
    // ANCOUNT = 1
    TEST_ASSERT_EQUAL_HEX8(0x00, out[6]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[7]);
    // Question section copied verbatim
    TEST_ASSERT_EQUAL_MEMORY(req + 12, out + 12, req_len - 12);
    // Answer trailer
    int pos = req_len;
    TEST_ASSERT_EQUAL_HEX8(0xC0, out[pos + 0]);
    TEST_ASSERT_EQUAL_HEX8(0x0C, out[pos + 1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[pos + 2]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[pos + 3]);  // type A
    TEST_ASSERT_EQUAL_HEX8(0x00, out[pos + 4]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[pos + 5]);  // class IN
    TEST_ASSERT_EQUAL_HEX8(0x00, out[pos + 10]);
    TEST_ASSERT_EQUAL_HEX8(0x04, out[pos + 11]); // rdlength 4
    TEST_ASSERT_EQUAL_UINT8(192, out[pos + 12]);
    TEST_ASSERT_EQUAL_UINT8(168, out[pos + 13]);
    TEST_ASSERT_EQUAL_UINT8(4,   out[pos + 14]);
    TEST_ASSERT_EQUAL_UINT8(1,   out[pos + 15]);
}

void test_bb_wifi_ap_dns_build_response_too_short_returns_zero(void)
{
    uint8_t req[11] = {0};  // < 12 bytes: no room for a DNS header
    const uint8_t answer_ip[4] = { 192, 168, 4, 1 };
    uint8_t out[64];
    int resp_len = bb_wifi_ap_dns_build_response(req, sizeof(req), answer_ip, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, resp_len);
}

void test_bb_wifi_ap_dns_build_response_exact_header_len(void)
{
    uint8_t req[12] = {0};  // exactly a bare header, no question
    const uint8_t answer_ip[4] = { 192, 168, 4, 1 };
    uint8_t out[64];
    int resp_len = bb_wifi_ap_dns_build_response(req, sizeof(req), answer_ip, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(12 + 16, resp_len);
}

void test_bb_wifi_ap_dns_build_response_out_cap_too_small_returns_zero(void)
{
    uint8_t req[64];
    int req_len = 0;
    fill_min_query(req, &req_len);
    const uint8_t answer_ip[4] = { 192, 168, 4, 1 };
    uint8_t out[8];  // far smaller than req_len + 16
    int resp_len = bb_wifi_ap_dns_build_response(req, req_len, answer_ip, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, resp_len);
}

// Exact-fit boundary: out_cap == req_len + 16 (the exact answer size) must
// still succeed and use the full buffer -- catches a `>` vs `>=` off-by-one
// in the resp_len > out_cap guard.
void test_bb_wifi_ap_dns_build_response_out_cap_exact_fit_succeeds(void)
{
    uint8_t req[64];
    int req_len = 0;
    fill_min_query(req, &req_len);
    const uint8_t answer_ip[4] = { 192, 168, 4, 1 };
    uint8_t out[64 + 16];
    int out_cap = req_len + 16;
    int resp_len = bb_wifi_ap_dns_build_response(req, req_len, answer_ip, out, out_cap);
    TEST_ASSERT_EQUAL_INT(req_len + 16, resp_len);
    TEST_ASSERT_EQUAL_INT(out_cap, resp_len);
}

// req_len above the accepted DNS query size ceiling (512) is rejected
// outright, independent of out_cap -- guards the int-overflow bound added
// for future direct callers passing an unvalidated req_len.
void test_bb_wifi_ap_dns_build_response_req_len_too_large_returns_zero(void)
{
    uint8_t req[600] = {0};
    const uint8_t answer_ip[4] = { 192, 168, 4, 1 };
    uint8_t out[1024];
    int resp_len = bb_wifi_ap_dns_build_response(req, 513, answer_ip, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, resp_len);
}

void test_bb_wifi_ap_dns_build_response_null_req_returns_zero(void)
{
    const uint8_t answer_ip[4] = { 192, 168, 4, 1 };
    uint8_t out[64];
    int resp_len = bb_wifi_ap_dns_build_response(NULL, 20, answer_ip, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, resp_len);
}

void test_bb_wifi_ap_dns_build_response_null_answer_ip_returns_zero(void)
{
    uint8_t req[64];
    int req_len = 0;
    fill_min_query(req, &req_len);
    uint8_t out[64];
    int resp_len = bb_wifi_ap_dns_build_response(req, req_len, NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, resp_len);
}

void test_bb_wifi_ap_dns_build_response_null_out_returns_zero(void)
{
    uint8_t req[64];
    int req_len = 0;
    fill_min_query(req, &req_len);
    const uint8_t answer_ip[4] = { 192, 168, 4, 1 };
    int resp_len = bb_wifi_ap_dns_build_response(req, req_len, answer_ip, NULL, 64);
    TEST_ASSERT_EQUAL_INT(0, resp_len);
}

void test_bb_wifi_ap_dns_build_response_different_answer_ip(void)
{
    uint8_t req[64];
    int req_len = 0;
    fill_min_query(req, &req_len);
    const uint8_t answer_ip[4] = { 10, 0, 0, 1 };
    uint8_t out[128];
    int resp_len = bb_wifi_ap_dns_build_response(req, req_len, answer_ip, out, sizeof(out));
    TEST_ASSERT_TRUE(resp_len > 0);
    TEST_ASSERT_EQUAL_UINT8(10, out[resp_len - 4]);
    TEST_ASSERT_EQUAL_UINT8(0,  out[resp_len - 3]);
    TEST_ASSERT_EQUAL_UINT8(0,  out[resp_len - 2]);
    TEST_ASSERT_EQUAL_UINT8(1,  out[resp_len - 1]);
}

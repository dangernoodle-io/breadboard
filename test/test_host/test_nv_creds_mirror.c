#include "unity.h"
#include "bb_nv_creds_mirror.h"
#include <string.h>

/* --- pack → valid round-trip --- */

void test_nv_creds_mirror_pack_valid_roundtrip(void)
{
    bb_nv_creds_mirror_t m;
    bb_nv_creds_mirror_pack(&m, "mynet", "s3cr3t", 1);
    TEST_ASSERT_TRUE(bb_nv_creds_mirror_valid(&m));
}

/* --- CRC detects mutations --- */

void test_nv_creds_mirror_flip_ssid_invalidates(void)
{
    bb_nv_creds_mirror_t m;
    bb_nv_creds_mirror_pack(&m, "mynet", "s3cr3t", 1);
    m.ssid[0] ^= 0x01;
    TEST_ASSERT_FALSE(bb_nv_creds_mirror_valid(&m));
}

void test_nv_creds_mirror_flip_pass_invalidates(void)
{
    bb_nv_creds_mirror_t m;
    bb_nv_creds_mirror_pack(&m, "mynet", "s3cr3t", 1);
    m.pass[0] ^= 0x01;
    TEST_ASSERT_FALSE(bb_nv_creds_mirror_valid(&m));
}

void test_nv_creds_mirror_flip_provisioned_invalidates(void)
{
    bb_nv_creds_mirror_t m;
    bb_nv_creds_mirror_pack(&m, "mynet", "s3cr3t", 1);
    m.provisioned ^= 0x01;
    TEST_ASSERT_FALSE(bb_nv_creds_mirror_valid(&m));
}

/* --- Field mismatches --- */

void test_nv_creds_mirror_magic_mismatch_invalid(void)
{
    bb_nv_creds_mirror_t m;
    bb_nv_creds_mirror_pack(&m, "mynet", "s3cr3t", 1);
    m.magic = 0xDEADBEEFu;
    TEST_ASSERT_FALSE(bb_nv_creds_mirror_valid(&m));
}

void test_nv_creds_mirror_version_mismatch_invalid(void)
{
    bb_nv_creds_mirror_t m;
    bb_nv_creds_mirror_pack(&m, "mynet", "s3cr3t", 1);
    /* bump version so it no longer matches BB_NV_CREDS_MIRROR_VERSION */
    m.version = BB_NV_CREDS_MIRROR_VERSION + 1u;
    TEST_ASSERT_FALSE(bb_nv_creds_mirror_valid(&m));
}

/* --- NULL guard --- */

void test_nv_creds_mirror_null_is_invalid(void)
{
    TEST_ASSERT_FALSE(bb_nv_creds_mirror_valid(NULL));
}

/* --- Truncation: oversized inputs are bounded + NUL-terminated --- */

void test_nv_creds_mirror_ssid_truncated_nul_terminated(void)
{
    /* ssid field is 32 bytes; an input of 40 chars must be silently truncated */
    bb_nv_creds_mirror_t m;
    char long_ssid[41];
    memset(long_ssid, 'A', 40);
    long_ssid[40] = '\0';

    bb_nv_creds_mirror_pack(&m, long_ssid, "pass", 1);

    /* must be NUL-terminated within the field */
    TEST_ASSERT_EQUAL_CHAR('\0', m.ssid[31]);
    /* last character before NUL must be the fill character */
    TEST_ASSERT_EQUAL_CHAR('A', m.ssid[30]);
    /* still structurally valid */
    TEST_ASSERT_TRUE(bb_nv_creds_mirror_valid(&m));
}

void test_nv_creds_mirror_pass_truncated_nul_terminated(void)
{
    /* pass field is 64 bytes; an input of 80 chars must be silently truncated */
    bb_nv_creds_mirror_t m;
    char long_pass[81];
    memset(long_pass, 'B', 80);
    long_pass[80] = '\0';

    bb_nv_creds_mirror_pack(&m, "net", long_pass, 1);

    TEST_ASSERT_EQUAL_CHAR('\0', m.pass[63]);
    TEST_ASSERT_EQUAL_CHAR('B', m.pass[62]);
    TEST_ASSERT_TRUE(bb_nv_creds_mirror_valid(&m));
}

/* --- NULL ssid / pass are treated as empty string --- */

void test_nv_creds_mirror_null_ssid_treated_as_empty(void)
{
    bb_nv_creds_mirror_t m;
    bb_nv_creds_mirror_pack(&m, NULL, "pass", 1);
    TEST_ASSERT_EQUAL_CHAR('\0', m.ssid[0]);
    TEST_ASSERT_TRUE(bb_nv_creds_mirror_valid(&m));
}

void test_nv_creds_mirror_null_pass_treated_as_empty(void)
{
    bb_nv_creds_mirror_t m;
    bb_nv_creds_mirror_pack(&m, "net", NULL, 1);
    TEST_ASSERT_EQUAL_CHAR('\0', m.pass[0]);
    TEST_ASSERT_TRUE(bb_nv_creds_mirror_valid(&m));
}

/* --- Empty SSID: structurally valid but not restorable ---
 *
 * The restore caller (step 2) must additionally check ssid[0] != '\0'.
 * The mirror itself is structurally valid; this test documents that contract.
 */
void test_nv_creds_mirror_empty_ssid_is_valid_but_not_restorable(void)
{
    bb_nv_creds_mirror_t m;
    bb_nv_creds_mirror_pack(&m, "", "pass", 1);

    /* structurally valid */
    TEST_ASSERT_TRUE(bb_nv_creds_mirror_valid(&m));
    /* but ssid[0] == '\0' signals no network configured */
    TEST_ASSERT_EQUAL_CHAR('\0', m.ssid[0]);
}

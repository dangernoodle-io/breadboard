#include "unity.h"
#include "bb_storage.h"
#include "bb_storage_rtc.h"
#include "bb_storage_rtc_region.h"
#include "bb_core.h"

#include <string.h>

// bb_storage_rtc's vtable (get/set/erase/exists) against the host stand-in
// region: keyed round-trip per key, the cold-boot/corrupt/version-mismatch
// "everything reads NOT_FOUND" guard, overflow -> NO_SPACE (never silent
// truncation), unknown-key handling, erase = whole-region invalidate, and
// warm survival across sequential gets without an intervening reset.

static void reset_all(void)
{
    bb_storage_test_reset();
    bb_storage_rtc_test_reset();
    bb_storage_rtc_register();
}

static bb_storage_addr_t addr_for(const char *key)
{
    bb_storage_addr_t addr = { .backend = "rtc", .ns_or_dir = NULL, .key = key };
    return addr;
}

/* ---------------------------------------------------------------------------
 * Cold boot: before any set(), every key reads NOT_FOUND.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_cold_boot_all_keys_not_found(void)
{
    reset_all();

    char buf[64];
    size_t out_len = 0;
    bb_storage_addr_t ssid_addr = addr_for("ssid");
    bb_storage_addr_t pass_addr = addr_for("pass");
    bb_storage_addr_t prov_addr = addr_for("provisioned");

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&ssid_addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&pass_addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&prov_addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_FALSE(bb_storage_exists(&ssid_addr));
    TEST_ASSERT_FALSE(bb_storage_exists(&pass_addr));
    TEST_ASSERT_FALSE(bb_storage_exists(&prov_addr));
}

/* ---------------------------------------------------------------------------
 * set -> get round trip, per key.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_ssid_set_get_round_trip(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, "mynet", 5));

    char buf[32] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(5, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("mynet", buf, 5);
    TEST_ASSERT_TRUE(bb_storage_exists(&addr));
}

void test_bb_storage_rtc_pass_set_get_round_trip(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for("pass");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, "s3cr3t", 6));

    char buf[64] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(6, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("s3cr3t", buf, 6);
    TEST_ASSERT_TRUE(bb_storage_exists(&addr));
}

void test_bb_storage_rtc_provisioned_set_get_round_trip(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for("provisioned");
    uint8_t val = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, &val, sizeof(val)));

    uint8_t out = 0;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&addr, &out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(sizeof(uint8_t), out_len);
    TEST_ASSERT_EQUAL_UINT8(1, out);
    TEST_ASSERT_TRUE(bb_storage_exists(&addr));
}

/* ---------------------------------------------------------------------------
 * bb_storage_rtc_test_reset() simulates a cold boot (power loss) -- every
 * key reads back NOT_FOUND again, even though bytes were previously written.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_test_reset_simulates_cold_boot(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, "mynet", 5));

    bb_storage_rtc_test_reset();

    char buf[32] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_FALSE(bb_storage_exists(&addr));
}

/* ---------------------------------------------------------------------------
 * Warm survival: two sequential gets WITHOUT a reset between them both see
 * the same valid, previously-set value.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_warm_survival_across_sequential_gets(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, "mynet", 5));

    char buf1[32] = {0};
    size_t out_len1 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&addr, buf1, sizeof(buf1), &out_len1));

    char buf2[32] = {0};
    size_t out_len2 = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&addr, buf2, sizeof(buf2), &out_len2));

    TEST_ASSERT_EQUAL(out_len1, out_len2);
    TEST_ASSERT_EQUAL_STRING_LEN(buf1, buf2, out_len1);
}

/* ---------------------------------------------------------------------------
 * CRC bit-flip corruption: subsequent get is NOT_FOUND, not a crash.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_crc_bitflip_invalidates_region(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, "mynet", 5));

    bb_storage_rtc_region_t *region = bb_storage_rtc_region_for_test();
    region->ssid[0] ^= 0x01;

    char buf[32] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_FALSE(bb_storage_exists(&addr));
}

/* ---------------------------------------------------------------------------
 * Version mismatch (e.g. an older/newer image's layout): subsequent get is
 * NOT_FOUND, not a crash.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_version_mismatch_invalidates_region(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, "mynet", 5));

    bb_storage_rtc_region_t *region = bb_storage_rtc_region_for_test();
    region->version = BB_STORAGE_RTC_REGION_VERSION + 1u;

    char buf[32] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_FALSE(bb_storage_exists(&addr));
}

/* ---------------------------------------------------------------------------
 * Overflow set: value longer than the field's capacity -> BB_ERR_NO_SPACE,
 * never silently truncated.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_ssid_overflow_set_returns_no_space(void)
{
    reset_all();

    char long_ssid[40];
    memset(long_ssid, 'A', sizeof(long_ssid));

    bb_storage_addr_t addr = addr_for("ssid");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_set(&addr, long_ssid, sizeof(long_ssid)));

    // The failed set must not have left a valid region behind.
    char buf[64] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&addr, buf, sizeof(buf), &out_len));
}

void test_bb_storage_rtc_pass_overflow_set_returns_no_space(void)
{
    reset_all();

    char long_pass[80];
    memset(long_pass, 'B', sizeof(long_pass));

    bb_storage_addr_t addr = addr_for("pass");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_set(&addr, long_pass, sizeof(long_pass)));
}

/* ---------------------------------------------------------------------------
 * provisioned set with the wrong length is a caller bug -> INVALID_ARG.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_provisioned_wrong_length_returns_invalid_arg(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for("provisioned");
    uint8_t two_bytes[2] = {1, 0};
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_set(&addr, two_bytes, sizeof(two_bytes)));
}

/* ---------------------------------------------------------------------------
 * Unknown key: get/exists -> NOT_FOUND/false, set/erase -> INVALID_ARG.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_unknown_key_get_is_not_found(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, "mynet", 5));

    bb_storage_addr_t unknown = addr_for("bogus");
    char buf[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&unknown, buf, sizeof(buf), &out_len));
    TEST_ASSERT_FALSE(bb_storage_exists(&unknown));
}

void test_bb_storage_rtc_unknown_key_set_is_invalid_arg(void)
{
    reset_all();

    bb_storage_addr_t unknown = addr_for("bogus");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_set(&unknown, "x", 1));
}

void test_bb_storage_rtc_unknown_key_erase_is_invalid_arg(void)
{
    reset_all();

    bb_storage_addr_t unknown = addr_for("bogus");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_erase(&unknown));
}

/* ---------------------------------------------------------------------------
 * erase() invalidates the WHOLE region -- every key reads back absent.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_erase_invalidates_whole_region(void)
{
    reset_all();

    bb_storage_addr_t ssid_addr = addr_for("ssid");
    bb_storage_addr_t pass_addr = addr_for("pass");
    bb_storage_addr_t prov_addr = addr_for("provisioned");

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ssid_addr, "mynet", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&pass_addr, "s3cr3t", 6));
    uint8_t val = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&prov_addr, &val, sizeof(val)));

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_erase(&ssid_addr));

    char buf[64] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&ssid_addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&pass_addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&prov_addr, buf, sizeof(buf), &out_len));
}

/* ---------------------------------------------------------------------------
 * Fail-open guard: a set() against an invalid region (simulating undefined
 * RTC_NOINIT bytes on true first power-up -- non-zero, non-NUL-terminated
 * garbage plus a garbage CRC) must zero the whole region before writing the
 * single field, never leave garbage in the untouched fields.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_set_on_invalid_region_zeroes_before_write(void)
{
    reset_all();

    bb_storage_rtc_region_t *region = bb_storage_rtc_region_for_test();
    memset(region, 0xAA, sizeof(*region));
    region->crc = 0xDEADBEEFu; /* still garbage / invalid */
    TEST_ASSERT_FALSE(bb_storage_rtc_region_valid(region));

    bb_storage_addr_t ssid_addr = addr_for("ssid");
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ssid_addr, "mynet", 5));

    // The other, untouched keys must read back safe -- never garbage.
    bb_storage_addr_t pass_addr = addr_for("pass");
    bb_storage_addr_t prov_addr = addr_for("provisioned");
    char buf[64] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&pass_addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(0, out_len);

    uint8_t prov_val = 0xFF;
    size_t prov_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&prov_addr, &prov_val, sizeof(prov_val), &prov_len));
    TEST_ASSERT_EQUAL_UINT8(0, prov_val);
}

/* ---------------------------------------------------------------------------
 * Sequential multi-key preservation: set(ssid), set(pass), set(provisioned)
 * on an already-valid region -- a later set() on a different key must never
 * clobber or wrongly re-zero the values from earlier sets.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_sequential_multi_key_sets_preserve_prior_keys(void)
{
    reset_all();

    bb_storage_addr_t ssid_addr = addr_for("ssid");
    bb_storage_addr_t pass_addr = addr_for("pass");
    bb_storage_addr_t prov_addr = addr_for("provisioned");

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ssid_addr, "mynet", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&pass_addr, "s3cr3t", 6));
    uint8_t val = 1;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&prov_addr, &val, sizeof(val)));

    char ssid_buf[32] = {0};
    size_t ssid_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ssid_addr, ssid_buf, sizeof(ssid_buf), &ssid_len));
    TEST_ASSERT_EQUAL(5, ssid_len);
    TEST_ASSERT_EQUAL_STRING_LEN("mynet", ssid_buf, 5);

    char pass_buf[64] = {0};
    size_t pass_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&pass_addr, pass_buf, sizeof(pass_buf), &pass_len));
    TEST_ASSERT_EQUAL(6, pass_len);
    TEST_ASSERT_EQUAL_STRING_LEN("s3cr3t", pass_buf, 6);
}

/* ---------------------------------------------------------------------------
 * NULL-key sentinel consistency: get/set/erase/exists all treat a NULL key
 * as a caller bug -- get/set/erase return BB_ERR_INVALID_ARG, exists returns
 * false (its only possible "not usable" signal).
 * ---------------------------------------------------------------------------*/
void test_bb_storage_rtc_null_key_sentinel_consistent(void)
{
    reset_all();

    bb_storage_addr_t addr = addr_for(NULL);
    char buf[16] = {0};
    size_t out_len = 0;

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get(&addr, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_set(&addr, "x", 1));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_erase(&addr));
    TEST_ASSERT_FALSE(bb_storage_exists(&addr));
}

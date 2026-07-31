// bb_log_secret — pins the masking policy of the ONLY sanctioned path for a
// secret-derived value (WiFi/MQTT password, PSK, token, ...) to reach a log
// line (see bb_log.h). Two classes of test:
//  (1) the helper itself never emits the raw secret substring -- captured
//      via freopen()-style redirection of the host backend's stdout/stderr
//      (bb_log_i/w/e write through plain fprintf on host, see bb_log.h's
//      non-ESP_PLATFORM branch);
//  (2) a source-scan guard proving the real call site
//      (platform/espidf/bb_wifi_ap/bb_wifi_ap.c, the exact line that shipped
//      the plaintext SSID/password leak) actually routes through
//      bb_log_secret rather than a bypassed hand-masked string -- a test
//      that only covered (1) would keep passing even if that call site
//      regressed back to a raw "password=%s" bb_log_i() call.
#include "unity.h"
#include "bb_log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

// A password value distinctive enough that an accidental substring match
// (e.g. against "***" or "(unset)") is not a risk, and that isn't a
// plausible real credential (placeholder only, never a real secret).
#define TEST_SECRET_VALUE "swordfish-not-a-real-secret-9182"

static char s_capture_buf[512];

// Fixed-arity args for the single generic capture callback below (no nested
// functions -- portable C, not a GNU extension).
static bb_log_level_t s_call_level;
static const char *s_call_secret;

static void call_bb_log_secret(void)
{
    bb_log_secret(s_call_level, "TAG", "password", s_call_secret);
}

// Capture everything written to `stream` during `fn()` into s_capture_buf.
// Redirected to a tmpfile() via dup2 so no filesystem path is needed and no
// host test output is lost permanently -- the original stream fd is
// restored before returning.
static void capture_stream(FILE *stream, void (*fn)(void))
{
    memset(s_capture_buf, 0, sizeof(s_capture_buf));

    FILE *tmp = tmpfile();
    TEST_ASSERT_NOT_NULL(tmp);

    int target_fd = fileno(stream);
    int saved_fd = dup(target_fd);
    TEST_ASSERT_TRUE(saved_fd >= 0);
    fflush(stream);
    // dup2() returns the (already-known) target fd number on success, not
    // 0 -- assert against target_fd, never a bare 0/1 literal.
    TEST_ASSERT_EQUAL_INT(target_fd, dup2(fileno(tmp), target_fd));

    fn();

    fflush(stream);
    dup2(saved_fd, target_fd);
    close(saved_fd);

    rewind(tmp);
    size_t n = fread(s_capture_buf, 1, sizeof(s_capture_buf) - 1, tmp);
    s_capture_buf[n] = '\0';
    fclose(tmp);
}

// ---------------------------------------------------------------------------
// (1) The helper itself: raw secret substring must be ABSENT, not merely
// "mask present" -- so a future change that appends the raw value after the
// mask still fails this test.
// ---------------------------------------------------------------------------

void test_bb_log_secret_info_never_contains_raw_value(void)
{
    s_call_level = BB_LOG_LEVEL_INFO;
    s_call_secret = TEST_SECRET_VALUE;
    capture_stream(stdout, call_bb_log_secret);
    TEST_ASSERT_NULL(strstr(s_capture_buf, TEST_SECRET_VALUE));
    TEST_ASSERT_NOT_NULL(strstr(s_capture_buf, "password=" BB_LOG_SECRET_MASK));
}

void test_bb_log_secret_error_never_contains_raw_value(void)
{
    s_call_level = BB_LOG_LEVEL_ERROR;
    s_call_secret = TEST_SECRET_VALUE;
    capture_stream(stderr, call_bb_log_secret);
    TEST_ASSERT_NULL(strstr(s_capture_buf, TEST_SECRET_VALUE));
    TEST_ASSERT_NOT_NULL(strstr(s_capture_buf, "password=" BB_LOG_SECRET_MASK));
}

void test_bb_log_secret_warn_never_contains_raw_value(void)
{
    s_call_level = BB_LOG_LEVEL_WARN;
    s_call_secret = TEST_SECRET_VALUE;
    capture_stream(stderr, call_bb_log_secret);
    TEST_ASSERT_NULL(strstr(s_capture_buf, TEST_SECRET_VALUE));
    TEST_ASSERT_NOT_NULL(strstr(s_capture_buf, "password=" BB_LOG_SECRET_MASK));
}

void test_bb_log_secret_null_secret_reports_unset(void)
{
    s_call_level = BB_LOG_LEVEL_INFO;
    s_call_secret = NULL;
    capture_stream(stdout, call_bb_log_secret);
    TEST_ASSERT_NULL(strstr(s_capture_buf, BB_LOG_SECRET_MASK));
    TEST_ASSERT_NOT_NULL(strstr(s_capture_buf, "password=(unset)"));
}

void test_bb_log_secret_empty_secret_reports_unset(void)
{
    s_call_level = BB_LOG_LEVEL_INFO;
    s_call_secret = "";
    capture_stream(stdout, call_bb_log_secret);
    TEST_ASSERT_NULL(strstr(s_capture_buf, BB_LOG_SECRET_MASK));
    TEST_ASSERT_NOT_NULL(strstr(s_capture_buf, "password=(unset)"));
}

// Mask width is fixed regardless of the secret's real length -- a
// length-preserving mask would leak how long the value is.
void test_bb_log_secret_mask_is_fixed_width_regardless_of_secret_length(void)
{
    s_call_level = BB_LOG_LEVEL_INFO;
    s_call_secret = TEST_SECRET_VALUE;  // 32 chars
    capture_stream(stdout, call_bb_log_secret);
    char long_buf[sizeof(s_capture_buf)];
    memcpy(long_buf, s_capture_buf, sizeof(long_buf));

    s_call_secret = "x";  // 1 char
    capture_stream(stdout, call_bb_log_secret);

    // Both must contain the SAME mask token -- the real secret's length
    // (32 chars vs 1 char) never shows up in the emitted mask.
    TEST_ASSERT_NOT_NULL(strstr(long_buf, "password=" BB_LOG_SECRET_MASK));
    TEST_ASSERT_NOT_NULL(strstr(s_capture_buf, "password=" BB_LOG_SECRET_MASK));
}

void test_bb_log_secret_none_level_emits_nothing(void)
{
    s_call_level = BB_LOG_LEVEL_NONE;
    s_call_secret = TEST_SECRET_VALUE;
    capture_stream(stdout, call_bb_log_secret);
    TEST_ASSERT_EQUAL_STRING("", s_capture_buf);
}

// DEBUG/VERBOSE both compile out to a no-op on the host backend (see
// bb_log.h's non-ESP_PLATFORM branch) -- these two only exercise the
// switch's DEBUG/VERBOSE cases for coverage, they can't assert on captured
// output the way the other levels do.
void test_bb_log_secret_debug_level_does_not_crash(void)
{
    s_call_level = BB_LOG_LEVEL_DEBUG;
    s_call_secret = TEST_SECRET_VALUE;
    capture_stream(stdout, call_bb_log_secret);
    TEST_PASS();
}

void test_bb_log_secret_verbose_level_does_not_crash(void)
{
    s_call_level = BB_LOG_LEVEL_VERBOSE;
    s_call_secret = TEST_SECRET_VALUE;
    capture_stream(stdout, call_bb_log_secret);
    TEST_PASS();
}

// Unknown/out-of-range level falls into `default` alongside NONE -- same
// "emit nothing" contract, mirrors test_bb_log_level_to_str_unknown's
// precedent for exercising this repo's usual out-of-range-enum case.
void test_bb_log_secret_unknown_level_emits_nothing(void)
{
    s_call_level = (bb_log_level_t)999;
    s_call_secret = TEST_SECRET_VALUE;
    capture_stream(stdout, call_bb_log_secret);
    TEST_ASSERT_EQUAL_STRING("", s_capture_buf);
}

// tag/label are forwarded into a printf-style "%s" internally -- a NULL
// value there is UB per the C standard (see bb_log.h's doc comment), so
// bb_log_secret must normalize NULL to "?" before that happens rather than
// pass it straight through. Asserts on the normalized "?" output, not just
// "does not crash", so a regression back to a raw pass-through is caught
// even on a libc (glibc) that happens to survive a NULL "%s" by printing
// "(null)".
static void call_bb_log_secret_null_tag_and_label(void)
{
    bb_log_secret(BB_LOG_LEVEL_INFO, NULL, NULL, TEST_SECRET_VALUE);
}

void test_bb_log_secret_null_tag_and_label_normalizes_instead_of_crashing(void)
{
    capture_stream(stdout, call_bb_log_secret_null_tag_and_label);
    TEST_ASSERT_NULL(strstr(s_capture_buf, TEST_SECRET_VALUE));
    TEST_ASSERT_NOT_NULL(strstr(s_capture_buf, "?=" BB_LOG_SECRET_MASK));
}

// ---------------------------------------------------------------------------
// (2) Call-site guard -- proves the real leak site
// (platform/espidf/bb_wifi_ap/bb_wifi_ap.c) routes the AP password through
// bb_log_secret rather than a plain bb_log_i("...password=%s...", pass)
// call. Source-scan rather than execution because that file is ESP-IDF-only
// (esp_wifi.h et al.) and has no host build. Path is relative to the
// worktree root, matching this repo's existing convention of running host
// tests with cwd = the PlatformIO project root (see make test / scripts/
// run_host_tests.py, which never chdir()s the compiled binary).
// ---------------------------------------------------------------------------

#define BB_WIFI_AP_SRC_PATH "platform/espidf/bb_wifi_ap/bb_wifi_ap.c"

// Sized generously above the current ~13KB source file so a reasonable
// amount of future growth in bb_wifi_ap.c doesn't silently truncate the
// scan (a truncated read that happens to cut off the target line would
// falsely FAIL this guard, not falsely pass it -- see the fread below).
static char s_source_buf[32768];

static size_t read_wifi_ap_source(void)
{
    FILE *f = fopen(BB_WIFI_AP_SRC_PATH, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not open " BB_WIFI_AP_SRC_PATH
                                     " -- run host tests with cwd at the worktree root");
    size_t n = fread(s_source_buf, 1, sizeof(s_source_buf) - 1, f);
    s_source_buf[n] = '\0';
    fclose(f);
    return n;
}

void test_bb_wifi_ap_start_routes_password_through_bb_log_secret(void)
{
    read_wifi_ap_source();
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_source_buf, "bb_log_secret(BB_LOG_LEVEL_INFO, TAG, \"password\""),
                                  "bb_wifi_ap.c must log the AP password via bb_log_secret()");
}

void test_bb_wifi_ap_start_never_hand_rolls_password_format(void)
{
    read_wifi_ap_source();
    // The exact pattern that shipped the plaintext leak: a %s conversion
    // fed directly by s_ap_password inside a plain bb_log_* format string.
    TEST_ASSERT_NULL_MESSAGE(strstr(s_source_buf, "password=%s"),
                              "bb_wifi_ap.c must never interpolate the AP password into a "
                              "printf-style format string -- route it through bb_log_secret() instead");
}

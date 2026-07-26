#include "unity.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "bb_config.h"
#include "fake_nvs_backend.h"

#include <string.h>

// bb_settings' durable "provisioned" flag (B1-807, corrected design) --
// the DURABLE NVS "bb_cfg"/"provisioned" key (BB_CONFIG_BOOL), distinct
// from bb_settings_wifi_rtc_mirror_provisioned_get's own volatile/
// degenerate RTC-mirror copy (see bb_settings.h). Same shared fake "nvs"
// bb_storage backend mechanism as test_bb_settings_wifi_pending.c /
// test_bb_settings_creds_write.c.
//
// Test-local field descriptor targeting the exact same addr (backend/ns/
// key) as bb_settings.c's internal s_wifi_provisioned_field, used only to
// seed/assert state independent of the real accessors under test.
static const bb_config_field_t s_test_provisioned_field = {
    .id = "wifi.provisioned", .type = BB_CONFIG_BOOL,
    .addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "provisioned" },
};

static void reset_all(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
    bb_settings_wifi_set_provisioned_cb(NULL);
}

// Fixture callback + fire counter -- mirrors test_bb_wifi.c's
// ota_validated_fixture pattern for the same bb_callback_slot idiom.
static int s_fire_count = 0;
static void provisioned_fixture(void) { s_fire_count++; }

/* ---------------------------------------------------------------------------
 * 2x2 matrix: has_creds x provisioned.
 * ---------------------------------------------------------------------------*/

// (1) fresh board -> false.
void test_bb_settings_wifi_provisioned_get_false_on_fresh_board(void)
{
    reset_all();
    TEST_ASSERT_FALSE(bb_settings_wifi_provisioned_get());
}

// (2) creds via bb_settings_wifi_set WITHOUT a validated connect -> still
// false. Regression guard against the RTC-mirror mis-wiring this ticket
// closes: bb_settings_wifi_set's own best-effort RTC mirror write hardcodes
// provisioned=1 on the MIRROR, but must NOT touch the durable NVS flag this
// getter reads.
void test_bb_settings_wifi_set_alone_does_not_provision(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("MyNetwork", "hunter2"));
    TEST_ASSERT_FALSE(bb_settings_wifi_provisioned_get());
}

// (3) pending_set + promote -> true, seam fired once.
void test_bb_settings_wifi_pending_promote_provisions_and_fires_once(void)
{
    reset_all();
    bb_settings_wifi_set_provisioned_cb(provisioned_fixture);
    s_fire_count = 0;

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    TEST_ASSERT_TRUE(bb_settings_wifi_provisioned_get());
    TEST_ASSERT_EQUAL(1, s_fire_count);

    bb_settings_wifi_set_provisioned_cb(NULL);
}

// (4) re-promote on an already-provisioned board -> stays true, seam does
// NOT fire again. This is the RED-if-inverted bite-proof: if the edge-gate
// in bb_settings_wifi_pending_promote were inverted (firing when
// was_provisioned==true instead of ==false), s_fire_count would read 2, not
// 1, after this second promote -- proving the assertion below actually
// exercises the "already true" branch, not just its lines.
void test_bb_settings_wifi_pending_promote_reconfigure_does_not_refire(void)
{
    reset_all();
    bb_settings_wifi_set_provisioned_cb(provisioned_fixture);
    s_fire_count = 0;

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());
    TEST_ASSERT_EQUAL(1, s_fire_count);

    // Second, independent reconfigure attempt on the now-provisioned board.
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("OtherNetwork", "swordfish"));
    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    TEST_ASSERT_TRUE(bb_settings_wifi_provisioned_get());
    TEST_ASSERT_EQUAL(1, s_fire_count);  // still 1 -- no refire

    bb_settings_wifi_set_provisioned_cb(NULL);
}

/* ---------------------------------------------------------------------------
 * direct-commit path: bb_settings_wifi_set (portal-style) followed by the
 * validated-connect marker (bb_wifi's GOT_IP handler, mirrored here via a
 * direct call).
 * ---------------------------------------------------------------------------*/

void test_bb_settings_wifi_provisioned_mark_connected_after_direct_commit(void)
{
    reset_all();
    bb_settings_wifi_set_provisioned_cb(provisioned_fixture);
    s_fire_count = 0;

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_set("MyNetwork", "hunter2"));
    TEST_ASSERT_FALSE(bb_settings_wifi_provisioned_get());  // not yet -- no connect yet

    bb_settings_wifi_provisioned_mark_connected();

    TEST_ASSERT_TRUE(bb_settings_wifi_provisioned_get());
    TEST_ASSERT_EQUAL(1, s_fire_count);

    bb_settings_wifi_set_provisioned_cb(NULL);
}

// A second validated connect on an already-provisioned board (e.g. a plain
// reboot-and-reconnect with no reconfigure in play) must not refire either.
void test_bb_settings_wifi_provisioned_mark_connected_idempotent(void)
{
    reset_all();
    bb_settings_wifi_set_provisioned_cb(provisioned_fixture);
    s_fire_count = 0;

    bb_settings_wifi_provisioned_mark_connected();
    TEST_ASSERT_EQUAL(1, s_fire_count);

    bb_settings_wifi_provisioned_mark_connected();
    TEST_ASSERT_EQUAL(1, s_fire_count);  // still 1 -- no refire
    TEST_ASSERT_TRUE(bb_settings_wifi_provisioned_get());

    bb_settings_wifi_set_provisioned_cb(NULL);
}

/* ---------------------------------------------------------------------------
 * failure case: forced promote failure -- flag unchanged, zero invocations.
 * ---------------------------------------------------------------------------*/

void test_bb_settings_wifi_pending_promote_failure_leaves_flag_unchanged(void)
{
    reset_all();
    bb_settings_wifi_set_provisioned_cb(provisioned_fixture);
    s_fire_count = 0;

    // No pending SSID staged -- BB_ERR_INVALID_STATE guard, nothing touched.
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_settings_wifi_pending_promote());

    TEST_ASSERT_FALSE(bb_settings_wifi_provisioned_get());
    TEST_ASSERT_EQUAL(0, s_fire_count);

    bb_settings_wifi_set_provisioned_cb(NULL);
}

// The OTHER promote failure branch: a pending ssid/pass/try IS staged (so
// the two read-guards above pass), but the atomic 3-key commit itself fails
// (platform/host/bb_settings/bb_settings.c's `if (err != BB_OK) return
// err;` right after bb_config_staged_commit). The provisioned-flag write
// that follows in source order must never run -- proven here by zero seam
// invocations, not just by code-reading the source order.
void test_bb_settings_wifi_pending_promote_commit_failure_leaves_flag_unchanged(void)
{
    reset_all();
    bb_settings_wifi_set_provisioned_cb(provisioned_fixture);
    s_fire_count = 0;

    TEST_ASSERT_EQUAL(BB_OK, bb_settings_wifi_pending_set("MyNetwork", "hunter2"));

    fake_nvs_backend_fail_commit();
    TEST_ASSERT_NOT_EQUAL(BB_OK, bb_settings_wifi_pending_promote());

    TEST_ASSERT_FALSE(bb_settings_wifi_provisioned_get());
    TEST_ASSERT_EQUAL(0, s_fire_count);

    bb_settings_wifi_set_provisioned_cb(NULL);
}

// The flag WRITE itself fails (bb_settings.c's
// `if (bb_config_set_bool(...) == BB_OK)` false path) -- the seam must not
// fire on a failed write, and the flag must read back false. The plain
// set() path already honors fake_nvs_backend_fail_set_key (same hook
// test_bb_settings_wifi_pending.c uses for the ssid/pass set()-failure
// branches), so no fake-backend change was needed for this one -- only for
// the atomic-commit branch above.
void test_bb_settings_wifi_provisioned_mark_connected_write_failure_leaves_flag_unchanged(void)
{
    reset_all();
    bb_settings_wifi_set_provisioned_cb(provisioned_fixture);
    s_fire_count = 0;

    fake_nvs_backend_fail_set_key("provisioned");
    bb_settings_wifi_provisioned_mark_connected();

    TEST_ASSERT_FALSE(bb_settings_wifi_provisioned_get());
    TEST_ASSERT_EQUAL(0, s_fire_count);

    bb_settings_wifi_set_provisioned_cb(NULL);
}

/* ---------------------------------------------------------------------------
 * fail-closed read: backend error -> getter returns false.
 * ---------------------------------------------------------------------------*/

void test_bb_settings_wifi_provisioned_get_false_on_backend_error(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_bool(&s_test_provisioned_field, true));

    fake_nvs_backend_fail_key("provisioned");
    TEST_ASSERT_FALSE(bb_settings_wifi_provisioned_get());
}

// Host tests for floor's provisioning-reboot guard (B1-809 review fix --
// HIGH: "reboot-loop guard has zero automated coverage"). examples/floor is
// an example, not a component, so it is not part of the native scaffold's
// component graph and cannot be reached the normal way -- this test
// #includes floor_prov_reboot.c directly (a relative path into
// examples/floor/main/), the SAME translation unit floor_app.c links
// against, not a hand-mirrored copy. See floor_prov_reboot.h's doc for the
// full contract and floor_app.c's wifi_lifecycle_observer for the live call
// site.
#include "unity.h"
#include "../../examples/floor/main/floor_prov_reboot.c"

#define SVC_WIFI ((bb_lifecycle_svc_t)0)
#define SVC_HTTP ((bb_lifecycle_svc_t)1)

// Genuine STOPPED->RUNNING edge, no-creds boot, not yet triggered: fires.
void test_floor_prov_reboot_fires_on_first_running_edge(void)
{
    TEST_ASSERT_TRUE(floor_should_trigger_prov_reboot(
        true, false, SVC_WIFI, SVC_WIFI, BB_LIFECYCLE_STOPPED, BB_LIFECYCLE_RUNNING));
}

// PAUSED->RUNNING is also a genuine entry edge: fires.
void test_floor_prov_reboot_fires_on_paused_to_running_edge(void)
{
    TEST_ASSERT_TRUE(floor_should_trigger_prov_reboot(
        true, false, SVC_WIFI, SVC_WIFI, BB_LIFECYCLE_PAUSED, BB_LIFECYCLE_RUNNING));
}

// A non-wifi service event (e.g. "http") must never trip this guard.
void test_floor_prov_reboot_wrong_service_no_fire(void)
{
    TEST_ASSERT_FALSE(floor_should_trigger_prov_reboot(
        true, false, SVC_HTTP, SVC_WIFI, BB_LIFECYCLE_STOPPED, BB_LIFECYCLE_RUNNING));
}

// A normal boot (creds already present, s_prov_mode_boot false) must never
// reboot on its own GOT_IP.
void test_floor_prov_reboot_normal_boot_no_fire(void)
{
    TEST_ASSERT_FALSE(floor_should_trigger_prov_reboot(
        false, false, SVC_WIFI, SVC_WIFI, BB_LIFECYCLE_STOPPED, BB_LIFECYCLE_RUNNING));
}

// One-shot latch: already triggered this boot must never fire again (guards
// against a flaky disconnect/reconnect re-fire queuing a second reboot).
void test_floor_prov_reboot_already_triggered_no_fire(void)
{
    TEST_ASSERT_FALSE(floor_should_trigger_prov_reboot(
        true, true, SVC_WIFI, SVC_WIFI, BB_LIFECYCLE_STOPPED, BB_LIFECYCLE_RUNNING));
}

// Bad creds that never reach RUNNING (e.g. STOPPED->PAUSED, still retrying)
// must never fire -- only a genuine RUNNING entry counts.
void test_floor_prov_reboot_non_running_new_state_no_fire(void)
{
    TEST_ASSERT_FALSE(floor_should_trigger_prov_reboot(
        true, false, SVC_WIFI, SVC_WIFI, BB_LIFECYCLE_STOPPED, BB_LIFECYCLE_PAUSED));
}

// RUNNING then a later RUNNING-again edge (old_state already RUNNING) is not
// a genuine entry edge -- must not fire (would otherwise fire on every
// no-op re-notify while already connected).
void test_floor_prov_reboot_running_to_running_no_fire(void)
{
    TEST_ASSERT_FALSE(floor_should_trigger_prov_reboot(
        true, false, SVC_WIFI, SVC_WIFI, BB_LIFECYCLE_RUNNING, BB_LIFECYCLE_RUNNING));
}

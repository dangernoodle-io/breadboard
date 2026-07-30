#include "unity.h"
#include "bb_http_prov_gate.h"

// Dedicated PlatformIO test env (native_prov_gate_disable, see
// platformio.ini) that builds WITH -DCONFIG_BB_WIFI_PROV_GATE_DISABLE=1 --
// the ONLY place in CI that actually compiles the
// bb_http_prov_gate_allow() escape-hatch branch (test_host's own env never
// defines this symbol, so that branch is preprocessed out there entirely --
// see bb_http_prov_gate.c). A typo in the #if guard, or a Kconfig rename,
// would otherwise leave the hatch silently inert or silently always-on with
// nothing catching it. Minimal smoke test only; does not re-run the full
// test_host suite.

void setUp(void) {}
void tearDown(void) {}

void test_prov_gate_disable_unconditionally_allows(void)
{
    // Empty allowlist + prov_active=true would deny under the normal
    // default-deny path; the compiled-in escape hatch must allow anyway.
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_GET, "/api/diag/meminfo"));
    TEST_ASSERT_TRUE(bb_http_prov_gate_allow(true, BB_HTTP_POST, "/anything"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_prov_gate_disable_unconditionally_allows);
    return UNITY_END();
}

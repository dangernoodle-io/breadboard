#pragma once

// Host-only test hooks for bb_mdns stubs.
//
// bb_mdns_announce() and bb_mdns_set_txt() are no-ops on host but increment
// counters so tests can assert call ordering and counts.
//
// Call bb_mdns_host_reset() in setUp() to zero all counters between tests.

int bb_mdns_host_announce_count(void);
int bb_mdns_host_set_txt_count(void);
void bb_mdns_host_reset(void);

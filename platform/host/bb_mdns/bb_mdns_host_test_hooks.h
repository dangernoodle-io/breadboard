#pragma once

#include "bb_mdns.h"
#include "bb_nv.h"

// Host-only test hooks for bb_mdns stubs.
//
// bb_mdns_announce() and bb_mdns_set_txt() are no-ops on host but increment
// counters so tests can assert call ordering and counts.
//
// Call bb_mdns_host_reset() in setUp() to zero all counters between tests.
//
// bb_mdns_host_dispatch_peer() and bb_mdns_host_dispatch_removed() invoke the
// registered callbacks synchronously (no queue/task on host), allowing tests
// to verify dispatch behaviour without hardware.

int bb_mdns_host_announce_count(void);
int bb_mdns_host_set_txt_count(void);
void bb_mdns_host_reset(void);

bb_err_t bb_mdns_host_dispatch_peer(const char *service, const char *proto,
                                    const bb_mdns_peer_t *peer);
bb_err_t bb_mdns_host_dispatch_removed(const char *service, const char *proto,
                                       const char *instance_name);

bb_err_t bb_mdns_host_dispatch_query_result(const bb_mdns_query_result_t *result,
                                            bb_mdns_query_cb cb, void *ctx);

#pragma once

// Host-only fault injection for bb_json allocation paths.
// Mirrors the bb_http_host_force_register_fail pattern.
//
// Call bb_json_host_force_alloc_fail_after(n) to make the (n+1)th
// bb_json_obj_new / bb_json_arr_new call return NULL. After one failure
// the counter resets to -1 (disabled) automatically.
// Pass -1 to disable without triggering a failure.

void bb_json_host_force_alloc_fail_after(int n);

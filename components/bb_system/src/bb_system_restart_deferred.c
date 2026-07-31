// bb_system_restart_deferred.c -- shared deferred-restart primitive,
// extracted from two hand-rolled call sites that each lazily created a
// private static bb_oneshot_timer_t + bb_timer_deferred_oneshot_create +
// bb_timer_oneshot_start(..., 500*1000) whose callback called
// bb_system_restart_reason(...): platform/espidf/bb_wifi/bb_wifi.c's
// reconfig_reboot_work_fn/bb_wifi_reconfigure, and examples/floor's
// prov_reboot_work_fn/wifi_lifecycle_observer. See bb_system.h's doc comment
// on bb_system_restart_deferred for the shared-timer last-arm-wins caveat
// this collapses the two idioms into.
//
// Portable: compiled on host AND ESP-IDF. bb_timer_deferred_oneshot_create/
// bb_timer_oneshot_start/bb_timer_oneshot_stop are already dispatched
// correctly per-platform (see platform/espidf/bb_timer and
// platform/host/bb_timer), so only the fired work_fn's actual restart call
// needs any platform awareness -- and that's handled by
// bb_system_restart_reason itself already being per-platform (host prints a
// diagnostic and exit(0)s; ESP-IDF persists the record and esp_restart()s).
#include "bb_system.h"
#include "bb_timer.h"
#include "bb_lock_once.h"
#include "bb_log.h"
#include <string.h>

#ifdef BB_SYSTEM_TESTING
#include "bb_system_test.h"
#endif

// Bounded to match bb_reboot_record_encode's 48-char detail bound (+ NUL) --
// see bb_system.h's bb_reboot_record_encode doc comment.
#define RESTART_DEFERRED_DETAIL_MAX 49

static const char *TAG = "bb_system_restart_deferred";

static bb_oneshot_timer_t s_restart_timer = NULL;
static bb_reset_source_t  s_pending_src;   // guarded by s_pending_lock -- see ensure_pending_lock()
static char               s_pending_detail[RESTART_DEFERRED_DETAIL_MAX]; // guarded by s_pending_lock

// s_pending_src/s_pending_detail are now written from TWO independent
// callers (bb_wifi_reconfigure and floor's provisioning arm) that can race
// each other -- see bb_system.h's bb_system_restart_deferred doc comment.
// s_pending_lock makes each call's "write pending state, then (re-)arm the
// timer" sequence atomic with respect to every other call, and also guards
// restart_deferred_work_fn's read of the same statics against a concurrent
// arm (see that function below for why the fire path needs the lock too).
// Lazily bb_lock_init()'d exactly once via bb_lock_once_ensure() -- same
// idiom as bb_lifecycle.c's ensure_lock().
static bb_lock_t s_pending_lock;
static bb_once_t s_pending_lock_once = BB_ONCE_INIT;

static inline bb_err_t ensure_pending_lock(void)
{
    bb_lock_config_t cfg = { .name = "bb_system_restart_deferred", .category = "system" };
    return bb_lock_once_ensure(&s_pending_lock_once, &cfg, &s_pending_lock);
}

#ifdef BB_SYSTEM_TESTING
// Host-test capture: mirrors bb_system_routes.c's s_reboot_capture pattern
// (capture the resolved args, skip the actual platform restart call, which
// on host would exit(0) the test process). See bb_system_test.h.
static struct {
    bool              called;
    bb_reset_source_t src;
    char              detail[RESTART_DEFERRED_DETAIL_MAX];
} s_restart_deferred_capture;

void bb_system_restart_deferred_capture_reset_for_test(void)
{
    memset(&s_restart_deferred_capture, 0, sizeof(s_restart_deferred_capture));
}

bool bb_system_restart_deferred_capture_get_for_test(bb_reset_source_t *out_src,
                                                       char *out_detail, size_t out_detail_size)
{
    if (!s_restart_deferred_capture.called) return false;
    if (out_src) *out_src = s_restart_deferred_capture.src;
    if (out_detail && out_detail_size > 0) {
        strncpy(out_detail, s_restart_deferred_capture.detail, out_detail_size - 1);
        out_detail[out_detail_size - 1] = '\0';
    }
    return true;
}

bb_oneshot_timer_t bb_system_restart_deferred_timer_for_test(void)
{
    return s_restart_timer;
}

bb_lock_t *bb_system_restart_deferred_pending_lock_for_test(void)
{
    return (ensure_pending_lock() == BB_OK) ? &s_pending_lock : NULL;  // LCOV_EXCL_BR_LINE -- lock-init failure not host-reproducible
}
#endif /* BB_SYSTEM_TESTING */

static void restart_deferred_work_fn(void *arg)
{
    (void)arg;

    // Copy the pending state out under the lock, then act on the local
    // copies unlocked. The fire path DOES need the lock, not just the arm
    // path: bb_timer_oneshot_stop()/_start() only prevent a FUTURE fire from
    // racing a new arm() call -- they cannot un-dequeue a fire that has
    // already been pulled off bb_timer_disp's queue and is executing this
    // function. A caller on another task can therefore genuinely call
    // bb_system_restart_deferred() (writing s_pending_detail byte-by-byte
    // via strncpy) at the same instant this function is reading it, which is
    // exactly the corrupted-mid-copy-string race the shared statics'
    // unsynchronized-read risk. Copying out under the lock, then releasing
    // it before the (possibly slow -- NVS write + esp_restart(), or the
    // BB_SYSTEM_TESTING capture) rest of this function keeps the critical
    // section itself tiny.
    bb_reset_source_t local_src;
    char              local_detail[RESTART_DEFERRED_DETAIL_MAX];

    bb_err_t lock_rc = ensure_pending_lock();
    // LCOV_EXCL_START -- bb_lock_init() failure is not host-reproducible
    // (same rationale as bb_lifecycle.c's ensure_lock() call sites); log
    // only, then fall through to the unconditional lock/unlock pair below --
    // bb_lock_lock()/unlock() are themselves safe (checked, no-op)
    // BB_ERR_INVALID_STATE returns on a never-initialized bb_lock_t, so this
    // never needs an `if (lock_rc == BB_OK)` guard around them. A device
    // this close to restarting must not be stranded by a lock-init failure,
    // so this reads unsynchronized (best-effort) rather than skipping the
    // restart.
    if (lock_rc != BB_OK) {
        bb_log_e(TAG, "restart_deferred fire: lock unavailable, reading pending state unsynchronized");
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&s_pending_lock);
    local_src = s_pending_src;
    strncpy(local_detail, s_pending_detail, sizeof(local_detail) - 1);
    local_detail[sizeof(local_detail) - 1] = '\0';
    bb_lock_unlock(&s_pending_lock);

#ifdef BB_SYSTEM_TESTING
    s_restart_deferred_capture.called = true;
    s_restart_deferred_capture.src = local_src;
    strncpy(s_restart_deferred_capture.detail, local_detail,
            sizeof(s_restart_deferred_capture.detail) - 1);
    s_restart_deferred_capture.detail[sizeof(s_restart_deferred_capture.detail) - 1] = '\0';
#endif
#ifdef ESP_PLATFORM
    bb_system_restart_reason(local_src, local_detail[0] ? local_detail : NULL);
#endif
}

void bb_system_restart_deferred(bb_reset_source_t src, const char *detail, uint32_t delay_us)
{
    bb_err_t lock_rc = ensure_pending_lock();
    // LCOV_EXCL_START -- bb_lock_init() failure is not host-reproducible; a
    // restart request must not be silently dropped because the lock could
    // not be created, so this falls through to the unconditional lock/unlock
    // pair below (safe no-op on a never-initialized bb_lock_t -- see the
    // matching comment in restart_deferred_work_fn) rather than refusing to
    // arm.
    if (lock_rc != BB_OK) {
        bb_log_e(TAG, "restart_deferred arm: lock unavailable, writing pending state unsynchronized");
    }
    // LCOV_EXCL_STOP
    bb_lock_lock(&s_pending_lock);

    s_pending_src = src;
    if (detail) {
        strncpy(s_pending_detail, detail, sizeof(s_pending_detail) - 1);
        s_pending_detail[sizeof(s_pending_detail) - 1] = '\0';
    } else {
        s_pending_detail[0] = '\0';
    }

    if (!s_restart_timer) {
        bb_timer_deferred_oneshot_create(restart_deferred_work_fn, NULL,
                                         "bb_system_restart_deferred", &s_restart_timer);
    }
    // Unconditional stop-before-start (no-op if not armed) preserves
    // bb_wifi_reconfigure's exact re-arm behavior; harmless no-op for the
    // provisioning site, which only ever arms once per boot. Kept inside the
    // lock so the "write pending state, then (re-)arm" sequence is atomic
    // with respect to any other caller's own write+arm sequence -- otherwise
    // two callers could interleave their writes with each other's arm call,
    // still corrupting-free thanks to the write-side lock above, but with an
    // unpredictable src/detail-vs-arm-order pairing.
    bb_timer_oneshot_stop(s_restart_timer);
    bb_timer_oneshot_start(s_restart_timer, delay_us);

    bb_lock_unlock(&s_pending_lock);
}

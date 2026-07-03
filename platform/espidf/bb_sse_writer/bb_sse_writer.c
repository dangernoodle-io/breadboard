#include "bb_sse_writer.h"
#include "../../../components/bb_sse_writer/src/bb_sse_idle.h"
#include "bb_http.h"
#include "bb_log.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_SSE_SEND_TIMEOUT_MS
#define BB_SSE_SEND_TIMEOUT_MS CONFIG_BB_SSE_SEND_TIMEOUT_MS
#endif
#ifdef CONFIG_BB_SSE_RECV_TIMEOUT_MS
#define BB_SSE_RECV_TIMEOUT_MS CONFIG_BB_SSE_RECV_TIMEOUT_MS
#endif
#ifdef CONFIG_BB_SSE_ABORT_POLL_MS
#define BB_SSE_ABORT_POLL_MS CONFIG_BB_SSE_ABORT_POLL_MS
#endif
#endif
#ifndef BB_SSE_SEND_TIMEOUT_MS
#define BB_SSE_SEND_TIMEOUT_MS 3000
#endif
#ifndef BB_SSE_RECV_TIMEOUT_MS
#define BB_SSE_RECV_TIMEOUT_MS 30000
#endif
#ifndef BB_SSE_ABORT_POLL_MS
#define BB_SSE_ABORT_POLL_MS 1000
#endif

static const char *TAG = "bb_sse_writer";

// Frame buffer size: must be large enough for the largest SSE frame any
// caller can produce (event_routes uses up to CONFIG_BB_EVENT_ROUTES_RING_MAX_ENTRY + 96).
// 1024 bytes covers all current callers with room to spare.
#define BB_SSE_FRAME_BUF 1024

void bb_sse_writer_run(bb_http_request_t *req,
                       const char *connected_line,
                       bb_sse_wait_fn_t wait_fn,
                       bb_sse_cleanup_fn_t cleanup_fn,
                       void *ctx,
                       uint32_t wait_timeout_ms,
                       uint32_t heartbeat_ms)
{
    int fd = bb_http_req_sockfd(req);

    // Receive timeout — surfaces dead peers via SO_RCVTIMEO.
    struct timeval tv = {
        .tv_sec  = BB_SSE_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (BB_SSE_RECV_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Send timeout — abandon stalled client sends so the socket/session is reclaimed.
    struct timeval tv_snd = {
        .tv_sec  = BB_SSE_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (BB_SSE_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd));

    // Disable Nagle: SSE frames are small and must arrive promptly.
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    bb_http_resp_set_type(req, "text/event-stream");
    bb_http_resp_set_header(req, "Cache-Control", "no-cache");
    bb_http_resp_set_header(req, "Connection", "keep-alive");
    bb_http_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    bb_http_resp_set_header(req, "Access-Control-Allow-Private-Network", "true");

    bb_err_t err = bb_http_resp_send_chunk(req, connected_line, -1);

    char buf[BB_SSE_FRAME_BUF];
    uint32_t idle_ms = 0;
    bool peer_dead = false;

    while (err == BB_OK) {
        // Slice the wait_fn/heartbeat interval into BB_SSE_ABORT_POLL_MS-sized
        // chunks so peer-abort is detected within ~BB_SSE_ABORT_POLL_MS instead
        // of waiting up to the full heartbeat_ms on a quiet topic (B1-517).
        // The sum of slices consumed equals wait_timeout_ms exactly, so
        // bb_sse_idle_advance below still sees the same step_ms it always
        // did — heartbeat cadence is unchanged.
        uint32_t remaining = wait_timeout_ms;
        int n = 0;
        for (;;) {
            // Peer-abort probe, delegated to bb_http (socket-lifecycle SSOT) —
            // bb_sse_writer never touches the raw fd.
            if (!bb_http_req_peer_alive(req)) {
                peer_dead = true;
                break;
            }

            uint32_t slice = bb_sse_abort_poll_slice_ms(remaining, BB_SSE_ABORT_POLL_MS);
            n = wait_fn(ctx, buf, sizeof(buf), slice);
            remaining -= slice;
            if (n != 0 || remaining == 0) {
                break;
            }
        }

        if (peer_dead) {
            err = BB_ERR_INVALID_STATE;
            break;
        }

        if (n > 0) {
            // Data ready — send frame, reset idle accumulator.
            err = bb_http_resp_send_chunk(req, buf, n);
            idle_ms = 0;
        } else if (n == 0) {
            // Idle timeout — accumulate and possibly ping.
            bool should_ping = false;
            idle_ms = bb_sse_idle_advance(idle_ms, wait_timeout_ms,
                                          heartbeat_ms, &should_ping);
            if (should_ping) {
                err = bb_http_resp_send_chunk(req, ": ping\n\n", -1);
            }
        } else {
            // Hard error from wait_fn — stop. Mark the fd as doomed (reusing
            // peer_dead: it now covers "peer dead OR fd doomed") so the
            // post-loop close-chunk send below is skipped (it would race
            // httpd's cleanup on a fd we no longer trust, the same
            // LoadProhibited class the peer-abort path guards against) and
            // teardown routes through bb_http_req_async_abort() (RST) below
            // instead of a graceful complete() that would leave the doomed
            // fd parked in CLOSE_WAIT.
            bb_log_d(TAG, "wait_fn returned %d, stopping SSE loop", n);
            err = BB_ERR_INVALID_STATE;
            peer_dead = true;
            break;
        }
    }

    // Only send close chunk when connection is believed alive — sending on a
    // dead fd races with httpd's cleanup and has caused LoadProhibited crashes.
    if (err == BB_OK) {
        bb_http_resp_send_chunk(req, NULL, 0);
    }

    if (cleanup_fn) {
        cleanup_fn(ctx);
    }

    // Peer-abort path: force an immediate RST-based close (via bb_http, the
    // socket-lifecycle SSOT) instead of the graceful FIN->CLOSE_WAIT teardown
    // a plain async_handler_complete would leave behind (B1-517). Never taken
    // for a graceful exit or heartbeat-only path — only a confirmed dead peer.
    if (peer_dead) {
        bb_http_req_async_abort(req);
    } else {
        bb_http_req_async_handler_complete(req);
    }
    vTaskDelete(NULL);
}

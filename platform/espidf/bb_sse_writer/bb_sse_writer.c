#include "bb_sse_writer.h"
#include "../../../components/bb_sse_writer/src/bb_sse_idle.h"
#include "bb_http.h"
#include "bb_log.h"

#include <errno.h>
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
#endif
#ifndef BB_SSE_SEND_TIMEOUT_MS
#define BB_SSE_SEND_TIMEOUT_MS 3000
#endif
#ifndef BB_SSE_RECV_TIMEOUT_MS
#define BB_SSE_RECV_TIMEOUT_MS 30000
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

    while (err == BB_OK) {
        // Peer-disconnect probe: peek without consuming. recv()==0 means peer
        // sent FIN; recv()==-1 with EAGAIN/EWOULDBLOCK means alive but quiet;
        // recv()==-1 with anything else (ECONNRESET, EBADF, ENOTCONN) is dead.
        char peek;
        ssize_t prc = recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (prc == 0) {
            err = BB_ERR_INVALID_STATE;  // peer FIN
            break;
        } else if (prc < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            err = BB_ERR_INVALID_STATE;  // fatal errno
            break;
        }

        int n = wait_fn(ctx, buf, sizeof(buf), wait_timeout_ms);
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
            // Hard error from wait_fn — stop.
            bb_log_d(TAG, "wait_fn returned %d, stopping SSE loop", n);
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

    bb_http_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

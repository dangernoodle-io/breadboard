#pragma once

// bb_wifi_creds — the wifi-credential-provider seam.
//
// Interface-only component: no implementation lives here. bb_settings (this
// repo's default provider) and any consumer-supplied provider (e.g. a
// tm_settings) both implement bb_wifi_creds_provider_t; a future bb_wifi
// takes a `const bb_wifi_creds_provider_t *` + ctx by injection. Nothing
// here self-registers — composition-only by construction (see the DI legacy
// fence in breadboard/CLAUDE.md).
//
// get_ssid/get_pass mirror bb_config_get_str's size-probe/truncation
// contract: cap=0 probes the true length via *out_len without touching buf;
// a short buf copies min(cap, len) bytes and still reports the true length
// in *out_len so the caller can detect truncation. The password value is
// secret — never log it.

#include "bb_core.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bb_err_t (*get_ssid)(void *ctx, char *buf, size_t cap, size_t *out_len);
    bb_err_t (*get_pass)(void *ctx, char *buf, size_t cap, size_t *out_len);
    bool     (*has_creds)(void *ctx);
    bb_err_t (*clear)(void *ctx);
} bb_wifi_creds_provider_t;

// Shared shape of get_ssid/get_pass -- also the shape a fallback (non-
// provider) reader must implement. Named so bb_wifi_creds_read (below) can
// take either a provider member function pointer or a fallback fn pointer
// through one parameter type.
typedef bb_err_t (*bb_wifi_creds_get_fn)(void *ctx, char *buf, size_t cap, size_t *out_len);

// Pure, host-testable dispatch: calls provider_fn(pctx, ...) if non-NULL,
// else fallback_fn(fctx, ...). Always passes a non-NULL out_len down to
// whichever function is called (a local on the caller's behalf when the
// caller's own out_len is NULL), so a provider relying on bb_config_get_str's
// contract (which rejects out_len==NULL) always gets a valid pointer and
// actually populates buf. This is the single call site bb_wifi's
// wifi_read_ssid/wifi_read_pass route through -- see bb_wifi.c.
bb_err_t bb_wifi_creds_read(bb_wifi_creds_get_fn provider_fn, void *pctx,
                             bb_wifi_creds_get_fn fallback_fn, void *fctx,
                             char *buf, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

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

#ifdef __cplusplus
}
#endif

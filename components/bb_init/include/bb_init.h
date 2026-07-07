#pragma once
#include <stddef.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bb_err_t (*bb_init_init_fn)(bb_http_handle_t server);

typedef struct {
    const char            *name;
    bb_init_init_fn    init;
    int                    order;
} bb_init_entry_t;

void    bb_init_add(const bb_init_entry_t *entry);
bb_err_t bb_init_init(void);
size_t  bb_init_count(void);
void    bb_init_foreach(void (*cb)(const bb_init_entry_t *, void *), void *ctx);
void    bb_init_clear(void);

// The constructor is global (not static) so each component's CMakeLists can
// add `-u bb_init_register__<name>` to force-keep the .o under PlatformIO,
// whose espidf builder strips IDF's WHOLE_ARCHIVE flag. See
// cmake/bb_init.cmake for the bb_init_force_register() helper.
//
// N is a sort key only — lower N runs first. It is NOT a route-count hint.
// Route counts are declared via bb_http_reserve_routes() in a companion
// BB_INIT_REGISTER_PRE_HTTP init function.
//
// DECOUPLING (KB #692): fn_ still takes a bb_http_handle_t server argument
// for source compatibility with every existing call site, but bb_init itself
// no longer resolves that handle — bb_init_init() passes NULL. This macro
// generates a per-component trampoline that ignores the incoming argument and
// resolves the real handle itself via bb_http_server_get_handle(), called
// from THIS translation unit (which must already depend on bb_http_server to
// make use of the server — e.g. to register routes). That keeps the
// bb_http_server_get_handle() reference out of bb_init entirely, so bb_init's
// CMakeLists carries no bb_http_server dependency and firmwares that never
// register a REGULAR-tier component (e.g. the serial-only floor, which only
// calls bb_init_init_early()) never link the web stack.
#define BB_INIT_REGISTER_N(name_, fn_, n_)                                 \
    static bb_err_t bb_init_trampoline__##name_(bb_http_handle_t server_unused_) { \
        (void)server_unused_;                                              \
        return (fn_)(bb_http_server_get_handle());                        \
    }                                                                      \
    static const bb_init_entry_t bb_init_entry__##name_ = {            \
        .name = #name_, .init = bb_init_trampoline__##name_, .order = (n_)     \
    };                                                                          \
    void bb_init_register__##name_(void) __attribute__((constructor));     \
    void bb_init_register__##name_(void) {                                 \
        bb_init_add(&bb_init_entry__##name_);                          \
    }

#define BB_INIT_REGISTER(name_, fn_) BB_INIT_REGISTER_N(name_, fn_, 0)

typedef bb_err_t (*bb_init_init_early_fn)(void);

typedef struct {
    const char                  *name;
    bb_init_init_early_fn    init;
    int                          order;
} bb_init_entry_early_t;

void     bb_init_add_early(const bb_init_entry_early_t *entry);
bb_err_t bb_init_init_early(void);
size_t   bb_init_count_early(void);
void     bb_init_foreach_early(void (*cb)(const bb_init_entry_early_t *, void *), void *ctx);
void     bb_init_clear_early(void);

// The constructor is global (not static) so each component's CMakeLists can
// add `-u bb_init_register_early__<name>` to force-keep the .o under PlatformIO,
// whose espidf builder strips IDF's WHOLE_ARCHIVE flag. See
// cmake/bb_init.cmake for the bb_init_force_register_early() helper.
#define BB_INIT_REGISTER_EARLY_N(name_, fn_, n_)                                  \
    static const bb_init_entry_early_t bb_init_entry_early__##name_ = {      \
        .name = #name_, .init = (fn_), .order = (n_)                                  \
    };                                                                                 \
    void bb_init_register_early__##name_(void) __attribute__((constructor));     \
    void bb_init_register_early__##name_(void) {                                 \
        bb_init_add_early(&bb_init_entry_early__##name_);                    \
    }

#define BB_INIT_REGISTER_EARLY(name_, fn_) BB_INIT_REGISTER_EARLY_N(name_, fn_, 0)

// PRE_HTTP tier — runs after EARLY, before the HTTP server starts.
// Signature is parallel to EARLY (no server arg).

typedef bb_err_t (*bb_init_init_pre_http_fn)(void);

typedef struct {
    const char                    *name;
    bb_init_init_pre_http_fn   init;
    int                            order;
} bb_init_entry_pre_http_t;

void     bb_init_add_pre_http(const bb_init_entry_pre_http_t *entry);
bb_err_t bb_init_init_pre_http(void);
size_t   bb_init_count_pre_http(void);
void     bb_init_foreach_pre_http(void (*cb)(const bb_init_entry_pre_http_t *, void *), void *ctx);
void     bb_init_clear_pre_http(void);

// The constructor is global (not static) so each component's CMakeLists can
// add `-u bb_init_register_pre_http__<name>` to force-keep the .o under PlatformIO,
// whose espidf builder strips IDF's WHOLE_ARCHIVE flag. See
// cmake/bb_init.cmake for the bb_init_force_register_pre_http() helper.
#define BB_INIT_REGISTER_PRE_HTTP_N(name_, fn_, n_)                                     \
    static const bb_init_entry_pre_http_t bb_init_entry_pre_http__##name_ = {      \
        .name = #name_, .init = (fn_), .order = (n_)                                        \
    };                                                                                      \
    void bb_init_register_pre_http__##name_(void) __attribute__((constructor));        \
    void bb_init_register_pre_http__##name_(void) {                                    \
        bb_init_add_pre_http(&bb_init_entry_pre_http__##name_);                    \
    }

#define BB_INIT_REGISTER_PRE_HTTP(name_, fn_) BB_INIT_REGISTER_PRE_HTTP_N(name_, fn_, 0)

#ifdef __cplusplus
}
#endif

#pragma once
#include <stddef.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bb_err_t (*bb_registry_init_fn)(bb_http_handle_t server);

typedef struct {
    const char            *name;
    bb_registry_init_fn    init;
    int                    route_count;
} bb_registry_entry_t;

void    bb_registry_add(const bb_registry_entry_t *entry);
bb_err_t bb_registry_init(void);
size_t  bb_registry_count(void);
void    bb_registry_foreach(void (*cb)(const bb_registry_entry_t *, void *), void *ctx);
void    bb_registry_clear(void);
size_t  bb_registry_route_count_total(void);

// The constructor is global (not static) so each component's CMakeLists can
// add `-u bb_registry_register__<name>` to force-keep the .o under PlatformIO,
// whose espidf builder strips IDF's WHOLE_ARCHIVE flag. See
// cmake/bb_registry.cmake for the bb_registry_force_register() helper.
#define BB_REGISTRY_REGISTER_N(name_, fn_, n_)                                 \
    static const bb_registry_entry_t bb_registry_entry__##name_ = {            \
        .name = #name_, .init = (fn_), .route_count = (n_)                     \
    };                                                                          \
    void bb_registry_register__##name_(void) __attribute__((constructor));     \
    void bb_registry_register__##name_(void) {                                 \
        bb_registry_add(&bb_registry_entry__##name_);                          \
    }

#define BB_REGISTRY_REGISTER(name_, fn_) BB_REGISTRY_REGISTER_N(name_, fn_, 0)

typedef bb_err_t (*bb_registry_init_early_fn)(void);

typedef struct {
    const char                  *name;
    bb_registry_init_early_fn    init;
} bb_registry_entry_early_t;

void     bb_registry_add_early(const bb_registry_entry_early_t *entry);
bb_err_t bb_registry_init_early(void);
size_t   bb_registry_count_early(void);
void     bb_registry_foreach_early(void (*cb)(const bb_registry_entry_early_t *, void *), void *ctx);
void     bb_registry_clear_early(void);

// The constructor is global (not static) so each component's CMakeLists can
// add `-u bb_registry_register_early__<name>` to force-keep the .o under PlatformIO,
// whose espidf builder strips IDF's WHOLE_ARCHIVE flag. See
// cmake/bb_registry.cmake for the bb_registry_force_register_early() helper.
#define BB_REGISTRY_REGISTER_EARLY(name_, fn_)                                       \
    static const bb_registry_entry_early_t bb_registry_entry_early__##name_ = {      \
        .name = #name_, .init = (fn_)                                                 \
    };                                                                                 \
    void bb_registry_register_early__##name_(void) __attribute__((constructor));     \
    void bb_registry_register_early__##name_(void) {                                 \
        bb_registry_add_early(&bb_registry_entry_early__##name_);                    \
    }

#ifdef __cplusplus
}
#endif

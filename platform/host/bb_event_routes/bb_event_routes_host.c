// Host port for bb_event_routes — pthread mutex per client slot.
// `notify` is a no-op on host: tests drive drain explicitly.
#include "bb_event_routes_internal.h"

#include <pthread.h>
#include <stdlib.h>

void *bb_event_routes_port_lock_create(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    if (!m) return NULL;
    if (pthread_mutex_init(m, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

void bb_event_routes_port_lock_destroy(void *lock)
{
    if (!lock) return;
    pthread_mutex_destroy((pthread_mutex_t *)lock);
    free(lock);
}

void bb_event_routes_port_lock(void *lock)
{
    if (lock) pthread_mutex_lock((pthread_mutex_t *)lock);
}

void bb_event_routes_port_unlock(void *lock)
{
    if (lock) pthread_mutex_unlock((pthread_mutex_t *)lock);
}

void bb_event_routes_port_notify(void *lock)
{
    (void)lock;  // no-op on host
}

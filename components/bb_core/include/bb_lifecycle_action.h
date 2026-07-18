#pragma once

/**
 * @brief Pure classification result for a lifecycle-driving producer (B1-1045
 * PR-1): the action a producer's emit-seam classifier tells a bound
 * bb_lifecycle service to take in response to one event. Mirrors bb_type.h's
 * single-enum shape.
 */

typedef enum {
    BB_LIFECYCLE_ACTION_NONE,  // no state change
    BB_LIFECYCLE_ACTION_START, // drive the bound service to started
    BB_LIFECYCLE_ACTION_STOP,  // drive the bound service to stopped
} bb_lifecycle_action_t;

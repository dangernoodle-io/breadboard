#pragma once

/**
 * @brief Shared field-value type tag used by descriptor-driven consumers
 * (e.g. bb_serialize) to describe a snapshot struct's field layout.
 */

typedef enum {
    BB_TYPE_I64,    // int64_t
    BB_TYPE_U64,    // uint64_t
    BB_TYPE_F64,    // double
    BB_TYPE_BOOL,   // bool
    BB_TYPE_STR,    // embedded NUL-terminated char[N], bounded read (strnlen)
    BB_TYPE_STR_N,  // ptr+len pair
    BB_TYPE_OBJ,    // nested struct (children/n_children)
    BB_TYPE_ARR,    // generic array (element shape via elem_type)
} bb_type_t;

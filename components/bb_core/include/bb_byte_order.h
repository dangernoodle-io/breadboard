#pragma once

// Portable byte-order helpers for reading/writing big-endian and little-endian
// multi-byte integers from unaligned byte buffers.
//
// Callers must never cast a byte buffer to uint32_t * or uint16_t * to read or
// write a multi-byte integer. Use these helpers instead. Aliased pointer casts
// hide endianness assumptions and have caused silent SHA-256 hash bugs in this
// project's consumers (see TaipanMiner TA-271, TA-322).
//
// All functions are alignment-safe: they work on buffers at any byte offset.
// Implementation uses portable shift-based loads and stores, which optimize to
// a single load/store + bswap instruction on modern compilers.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load a 32-bit big-endian integer from an unaligned byte buffer.
static inline uint32_t bb_load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Load a 32-bit little-endian integer from an unaligned byte buffer.
static inline uint32_t bb_load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Load a 16-bit big-endian integer from an unaligned byte buffer.
static inline uint16_t bb_load_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

// Load a 16-bit little-endian integer from an unaligned byte buffer.
static inline uint16_t bb_load_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Store a 32-bit big-endian integer to an unaligned byte buffer.
static inline void bb_store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Store a 32-bit little-endian integer to an unaligned byte buffer.
static inline void bb_store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Store a 16-bit big-endian integer to an unaligned byte buffer.
static inline void bb_store_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

// Store a 16-bit little-endian integer to an unaligned byte buffer.
static inline void bb_store_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

#ifdef __cplusplus
}
#endif

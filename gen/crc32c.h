/* crc32c.h: CRC32C (Castagnoli) with the CPU's CRC instructions.
 *
 * crc32c_u64/u32/u8(crc, value) fold value into crc and return the new crc,
 * exactly like _mm_crc32_u64/_u32/_u8 on x86-64 (SSE 4.2) and
 * __crc32cd/__crc32cw/__crc32cb on AArch64 (CRC extension): both give
 * identical results.  No initial or final inversion is applied; do it yourself
 * if you want the standard CRC32C value (crc = ~0, ..., ~crc).
 *
 * Build with -msse4.2 on x86-64 and -march=armv8-a+crc on AArch64 (the
 * Makefile does it).
 */
#ifndef CRC32C_H
#define CRC32C_H

#include <stdint.h>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
static inline uint32_t crc32c_u64(uint32_t crc, uint64_t v) { return (uint32_t)_mm_crc32_u64(crc, v); }
static inline uint32_t crc32c_u32(uint32_t crc, uint32_t v) { return _mm_crc32_u32(crc, v); }
static inline uint32_t crc32c_u8 (uint32_t crc, uint8_t  v) { return _mm_crc32_u8(crc, v); }
#elif defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
static inline uint32_t crc32c_u64(uint32_t crc, uint64_t v) { return __crc32cd(crc, v); }
static inline uint32_t crc32c_u32(uint32_t crc, uint32_t v) { return __crc32cw(crc, v); }
static inline uint32_t crc32c_u8 (uint32_t crc, uint8_t  v) { return __crc32cb(crc, v); }
#else
#error "crc32c.h needs SSE 4.2 (-msse4.2) on x86-64 or the CRC extension (-march=armv8-a+crc) on AArch64"
#endif

#endif /* CRC32C_H */

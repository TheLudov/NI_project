/* crc32c.h: the standard check value, and u64/u32/u8 give the same CRC over the same bytes (little endian). */
#include <stdio.h>
#include <string.h>

#include "crc32c.h"

int main(void)
{
    const char *s = "123456789";
    uint32_t c = ~0u;
    for (int i = 0; s[i]; i++) c = crc32c_u8(c, (uint8_t)s[i]);
    if (~c != 0xe3069283u) {
        printf("test_crc32c: FAIL: CRC32C(\"123456789\") = %08x, want e3069283\n", ~c);
        return 1;
    }
    uint8_t b[64];
    for (int i = 0; i < 64; i++) b[i] = (uint8_t)(i * 37 + 11);
    uint32_t c8 = 0x12345678u, c32 = c8, c64 = c8;
    for (int i = 0; i < 64; i++) c8 = crc32c_u8(c8, b[i]);
    for (int i = 0; i < 64; i += 4) { uint32_t w; memcpy(&w, b + i, 4); c32 = crc32c_u32(c32, w); }
    for (int i = 0; i < 64; i += 8) { uint64_t w; memcpy(&w, b + i, 8); c64 = crc32c_u64(c64, w); }
    if (c8 != c32 || c8 != c64) {
        printf("test_crc32c: FAIL: u8 %08x, u32 %08x, u64 %08x differ\n", c8, c32, c64);
        return 1;
    }
    printf("test_crc32c: ok\n");
    return 0;
}

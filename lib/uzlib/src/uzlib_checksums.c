#include "uzlib.h"

uint32_t uzlib_adler32(const void *data, unsigned int length, uint32_t prev_sum) {
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t s1 = prev_sum & 0xffff;
    uint32_t s2 = (prev_sum >> 16) & 0xffff;
    for (unsigned int i = 0; i < length; i++) {
        s1 = (s1 + buf[i]) % 65521;
        s2 = (s2 + s1)     % 65521;
    }
    return (s2 << 16) | s1;
}

uint32_t uzlib_crc32(const void *data, unsigned int length, uint32_t crc) {
    static const uint32_t table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *buf = (const uint8_t *)data;
    crc = ~crc;
    for (unsigned int i = 0; i < length; i++) {
        crc = (crc >> 4) ^ table[(crc ^ (buf[i] >> 0)) & 0x0f];
        crc = (crc >> 4) ^ table[(crc ^ (buf[i] >> 4)) & 0x0f];
    }
    return ~crc;
}

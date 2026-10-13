#include "protocol.h"
uint16_t a1_crc16(uint16_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

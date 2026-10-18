#include "../M1/scd40.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* Published Sensirion CRC example: BE EF -> 92. */
    uint8_t example[] = {0xbe, 0xef, 0x92};
    uint16_t words[3];
    assert(scd40_crc(example) == 0x92);
    assert(scd40_words(example, words, 1) && words[0] == 0xbeef);
    uint8_t sample[9] = {0x03, 0x20, 0, 0x66, 0x66, 0, 0x80, 0, 0};
    for (unsigned i = 0; i < 3; i++) sample[i * 3 + 2] = scd40_crc(sample + i * 3);
    assert(scd40_words(sample, words, 3));
    assert(words[0] == 800 && words[1] == 26214 && words[2] == 32768);
    /* Reject every single-bit error across all three data words and CRCs. */
    for (unsigned bit = 0; bit < 72; bit++) {
        sample[bit / 8] ^= 1u << (bit % 8);
        assert(!scd40_words(sample, words, 3));
        sample[bit / 8] ^= 1u << (bit % 8);
    }
    puts("SCD40 CRC known vector, word ordering and 72 corruption cases passed");
}

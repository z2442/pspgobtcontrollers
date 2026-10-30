#include "xmb_patch.h"
#include <string.h>

int gobt_patch_filter(uint32_t firmware, uint8_t *text, size_t size)
{
    uint32_t a, b;
    const size_t first = 0x13d40, second = 0x13d6c;
    if (firmware != 0x06060010 && firmware != 0x06060110)
        return -1;
    if (!text || size < second + 4)
        return -2;
    memcpy(&a, text + first, 4);
    memcpy(&b, text + second, 4);
    if (a == 0x240effff && b == 0x144e0003)
        return 1;
    /* btfree's 6.6x offsets and documented original MIPS instructions.
     * Both must match before either store. Never patch an unknown image. */
    if (a != 0x240e0005 || b != 0x104e0003)
        return -3;
    a = 0x240effff; b = 0x144e0003;
    memcpy(text + first, &a, 4);
    memcpy(text + second, &b, 4);
    return 0;
}

#include "mapping_patch.h"
#include <string.h>

static uint32_t word(const uint8_t *text, unsigned offset)
{
    uint32_t value;
    memcpy(&value, text + offset, 4);
    return value;
}

static uint32_t jump(uint32_t address, int link)
{
    return (link ? 0x0c000000u : 0x08000000u) | ((address >> 2) & 0x03ffffffu);
}

static int pair(const uint8_t *text, unsigned upper, unsigned lower, uint32_t hi,
                uint32_t lo, uint32_t address)
{
    return word(text, upper) == (hi | ((address + 0x8000u) >> 16)) &&
           word(text, lower) == (lo | (address & 0xffffu));
}

int gobt_mapping_supported(uint32_t firmware, const uint8_t *text, size_t size, uint32_t base)
{
    static const struct { unsigned offset; uint32_t value; } fixed[] = {
        {0x108dc,0x27bdff90}, {0x10900,0x00808821}, {0x1098c,0x8c620194},
        {0x10998,0x8e430004}, {0x109a0,0x8e440018},
        {0x10a88,0x8fa60000}, {0x10a8c,0x02c02821}, {0x10a94,0x8c620168},
        {0x10aa8,0x8cc30048}, {0x11e4,0x00802821}, {0x11ec,0x27bdfff0},
        {0x5b4,0x27bdfff0}, {0x5c0,0x8c840004},
        {0x67d0,0x27bdfff0}, {0x67e4,0x24840034}, {0x67ec,0x2604004c},
        {0x67f0,0xae000048}, {0x67f4,0xae000044},
        {0x6f30,0x27bdffc0}, {0x6f44,0x2610004c}, {0x6f80,0xafb1000c}, {0x6f88,0xafb20010},
        {0x12104,0x24020008}, {0x12134,0x24020009}, {0x12138,0xae020014},
        {0x12140,0xae000018}, {0x12144,0xae00001c},
        {0x123dc,0x8c900014}, {0x123f8,0x24540004}, {0x123fc,0xae90fffc},
        {0x12420,0xae740018},
        /* Close completion: clear and notify via +0x94, reset flags last. */
        {0x91cc,0x27bdfff0}, {0x91e4,0xac800028}, {0x91e8,0x8c620090},
        {0x91fc,0x8c620094}, {0x920c,0x8c6201f4}, {0x9218,0x8e22002c},
        {0x9220,0x30430008}, {0x9230,0xae20002c},
        /* Native open: closing check, then build/attach/focus. */
        {0x9dd4,0x27bdffd0}, {0x9df4,0x8c82002c}, {0x9dfc,0x30420004},
        {0x9e00,0x10400008}, {0x9e28,0x00003021}, {0x9e48,0x2604001c},
        {0x9e64,0x8c620170}, {0x9e68,0x0040f809}, {0x9e7c,0x8c620178},
        {0x9e90,0x8c620048}, {0x9ea0,0x8c62019c},
        /* Close begin marks closing; detach clears the attached page. */
        {0x9f00,0x34420004}, {0x3a64,0x8c900008}, {0x3a78,0xae200008}
    };
    unsigned i;
    if ((firmware != 0x06060010 && firmware != 0x06060110) || !text ||
        size < 0x20c68 || base < 0x08800000u || base > 0x0c000000u - 0x20e00u || (base & 3)) return 0;
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
        if (word(text, fixed[i].offset) != fixed[i].value) return 0;
    if (!pair(text, 0x10a80, 0x10a84, 0x3c100000, 0x8e040000, base + 0x20d78) ||
        !pair(text, 0x10974, 0x109ac, 0x3c100000, 0x8e100000, base + 0x20d70) ||
        !pair(text, 0x11e8, 0x11f8, 0x3c040000, 0x24840000, base + 0x20d0c) ||
        word(text, 0x11f4) != jump(base + 0x5b4, 1) ||
        word(text, 0x6f54) != jump(base + 0x11e4, 1) ||
        (word(text, 0x6f60) & 0xffff0000u) != 0x3c020000u ||
        (word(text, 0x6f68) & 0xffff0000u) != 0x24420000u ||
        word(text, 0x6f70) != jump(base + GOBT_MAPPING_WIDE_CTOR, 1) ||
        word(text, 0x6f84) != jump(base + GOBT_MAPPING_VECTOR_PUSH, 1) ||
        word(text, 0x6f8c) != jump(base + GOBT_MAPPING_WIDE_DTOR, 1) ||
        word(text, 0x5bc) != jump(base + 0x1ab68, 1) ||
        word(text, 0x67e0) != jump(base + 0x16be4, 1) ||
        word(text, 0x67e8) != jump(base + 0x16c38, 1) ||
        word(text, GOBT_MAPPING_GENERIC_JUMP) != jump(base + GOBT_MAPPING_APPEND + 4, 0) ||
        (word(text, GOBT_MAPPING_GENERIC_JUMP + 4) & 0xffff0000u) != 0x3c100000u ||
        word(text, 0x7c48) != jump(base + 0x9ee8, 1) ||
        word(text, 0x79d8) != jump(base + GOBT_MAPPING_CLEAR, 1)) return 0;
    if (word(text, GOBT_MAPPING_VTABLE + GOBT_MAPPING_CLOSE_SLOT) != base + GOBT_MAPPING_CLOSE_DONE ||
        word(text, GOBT_MAPPING_VTABLE + 0x88) != base + 0x7c40 ||
        word(text, GOBT_MAPPING_VTABLE + 0x94) != base + 0x79c4 ||
        word(text, GOBT_MAPPING_VTABLE + 0x48) != base + 0x39f4 ||
        word(text, GOBT_MAPPING_VTABLE + 0x19c) != base + 0x7cbc ||
        word(text, 0x1fd58 + 0x180) != base + 0x6f30 ||
        word(text, 0x1fd58 + 0x170) != base + 0x108dc ||
        word(text, 0x1fd58 + 0x16c) != base + 0x10bc0 ||
        memcmp(text + 0x1c41c, "page_bt_config_registered_option", 32)) return 0;
    return 1;
}

int gobt_mapping_build_stub(uint32_t *code, uint32_t address, uint32_t callback,
                            uint32_t base, const uint32_t displaced[2])
{
    if (!code || !displaced || (address & 3) || (callback & 3) ||
        ((address ^ callback) & 0xf0000000u) || ((address ^ base) & 0xf0000000u)) return -1;
    code[0] = 0x27bdffe0; /* addiu sp,-32; keep the O32 argument area. */
    code[1] = 0xafbf001c;
    code[2] = 0x02202021; /* menu = s1 */
    code[3] = 0x02002821; /* selected device widget = s0 */
    code[4] = 0x02c03021; /* parent = s6 */
    code[5] = jump(callback, 1);
    code[6] = 0;
    code[7] = 0x8fbf001c;
    code[8] = 0x27bd0020;
    code[9] = displaced[0];
    code[10] = displaced[1];
    code[11] = jump(base + GOBT_MAPPING_APPEND + 8, 0);
    code[12] = 0;
    return 0;
}

int gobt_mapping_build_bypass(uint32_t *code, uint32_t address, uint32_t base,
                              uint32_t displaced)
{
    if (!code || (address & 3) || ((address ^ base) & 0xf0000000u)) return -1;
    code[0] = displaced; /* lw a0,page(s0); s0 was set in the jump's delay slot. */
    code[1] = jump(base + GOBT_MAPPING_APPEND + 8, 0);
    code[2] = 0;
    return 0;
}

#include "pairing_route.h"
#include "report_bridge.h"
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int gobt_pair_scan_record(const uint8_t *record, size_t size)
{
    if (!record || size < GOBT_SCAN_RECORD_BYTES ||
        le32(record) != GOBT_SCAN_RECORD_BYTES) return 0;
    return gobt_controller_model(record, size, GOBT_SCAN_RECORD_BYTES) != GOBT_MODEL_NONE;
}

int gobt_pair_default_parameter(uint8_t *parameter, size_t size)
{
    unsigned i;
    if (!parameter || size < GOBT_PAIR_PARAMETER_BYTES) return -1;
    memset(parameter, 0, GOBT_PAIR_PARAMETER_BYTES);
    /* Match the native XMB constructor's four UTF-16 U+0030 code units. */
    for (i = 0; i < 4; ++i) parameter[2 * i] = '0';
    return 0;
}

int gobt_pair_route_supported(uint32_t firmware, const uint8_t *text,
                              size_t size, uint32_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        /* Fallback registration: a0=object, a1=object+79 in the delay slot. */
        {0x13934, 0x27bdfff0}, {0x13938, 0xafbf0000},
        {0x13940, 0x2485004f}, {0x13944, 0x8fbf0000},
        {0x13948, 0x03e00008}, {0x1394c, 0x27bd0010},
        /* Existing full-record registration passes object+4 and object+88. */
        {0x13b28, 0x27bdfff0}, {0x13b2c, 0x24860058},
        {0x13b30, 0xafbf0000}, {0x13b38, 0x24850004},
        /* Its helper forwards the scan record and parameter to sceBSMan. */
        {0x138c8, 0x27bdfff0}, {0x138cc, 0x00a02021},
        {0x138d8, 0x00c02821},
        /* Normal XMB pairing parameter initialization: four UTF-16 zeros. */
        {0x13a5c, 0x24030030}, {0x13a60, 0x24020003},
        {0x13a64, 0x2442ffff}, {0x13a68, 0xa6030000},
        {0x13a6c, 0x0441fffd}, {0x13a70, 0x26100002},
    };
    unsigned i;
    uint32_t call;
    if (firmware != 0x06060010 && firmware != 0x06060110) return 0;
    if (!text || size < 0x13b3c || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    call = 0x0c000000u | (((base + GOBT_PAIR_ORIGINAL_OFFSET) >> 2) & 0x03ffffffu);
    return le32(text + GOBT_PAIR_CALL_OFFSET) == call;
}

int gobt_pair_commands_supported(uint32_t firmware, const uint8_t *text,
                                 size_t size, uint32_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        {0x3720, 0x27bdffc0}, {0x3724, 0xafb10024},
        {0x372c, 0xafb50034},
        {0x6758, 0x27bdff50},
        {0x67e0, 0x24060062}, {0x67e8, 0x24020022},
        {0x688c, 0x02a02021}, {0x6890, 0x02602821},
        {0x6894, 0x00003021}, {0x68a4, 0xafb20058},
        {0x69c0, 0x02a02021}, {0x69c4, 0x02602821},
        {0x69d0, 0x240b0065}, {0x69d8, 0x02203021},
        {0x69f8, 0xafbd0044},
    };
    uint32_t call, state = base + 0xdd00u;
    unsigned i;
    if (firmware != 0x06060010 && firmware != 0x06060110) return 0;
    if (!text || size < 0x69fc || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    if (le32(text + 0x3728) != (0x3c110000u | (((state + 0x8000u) >> 16) & 0xffffu)) ||
        le32(text + 0x3730) != (0x26350000u | (state & 0xffffu))) return 0;
    call = 0x0c000000u | (((base + GOBT_PAIR_COMMAND_OFFSET) >> 2) & 0x03ffffffu);
    return le32(text + GOBT_PAIR_PARAMETER_CALL_OFFSET) == call &&
           le32(text + GOBT_PAIR_START_CALL_OFFSET) == call;
}

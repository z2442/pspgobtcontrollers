#include "hid_route.h"
#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int gobt_route_hid_record(uint8_t *profile, size_t profile_size,
                         const uint8_t *info, size_t info_size)
{
    static const char prefix[] = "DualSense";
    unsigned i;
    uint32_t kind;
    uint16_t next;
    if (!profile || !info || profile_size < GOBT_PROFILE_BYTES ||
        info_size < GOBT_DEVICE_INFO_BYTES) return 0;
    kind = le32(profile + 4);
    if (le32(info) != GOBT_DEVICE_INFO_BYTES || kind > GOBT_PROFILE_GENERIC_HID ||
        le16(info + 0x48) != 0x0402) return 0;
    for (i = 0; i < sizeof(prefix) - 1; ++i)
        if (le16(info + 6 + i * 2) != (uint8_t)prefix[i]) return 0;
    next = le16(info + 6 + (sizeof(prefix) - 1) * 2);
    if (next != 0 && next != ' ') return 0;
    /* This mode is outside Sony's 0..8 enum. Its label and transport methods
     * are handled explicitly, including saved-device construction. */
    profile[4] = GOBT_PROFILE_GENERIC_HID;
    profile[5] = profile[6] = profile[7] = 0;
    return 1;
}

int gobt_route_hid_cached(uint8_t *profile, size_t size)
{
    if (!profile || size < GOBT_PROFILE_BYTES || le32(profile + 4) > GOBT_PROFILE_GENERIC_HID) return 0;
    profile[4] = GOBT_PROFILE_GENERIC_HID;
    profile[5] = profile[6] = profile[7] = 0;
    return 1;
}

int gobt_hid_mode_supported(const uint8_t *text, size_t size, uint32_t base)
{
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0x153fc, 0x00801021}, {0x15400, 0x27bdfff0},
        {0x15404, 0x00a02021}, {0x15408, 0xafbf0000}, {0x15410, 0x00402821},
        {0x151a8, 0x00808021}, {0x151e0, 0x02202821},
        {0x151e4, 0x04400029}, {0x15224, 0x24020003},
        {0x15228, 0x10600009}, {0x152dc, 0x00409021},
        {0x146c4, 0x27bdfff0}, {0x146c8, 0xafbf0000},
        {0x14d34, 0x27bdff60}, {0x14d38, 0xafb40090},
        {0x15018, 0x27bdffc0}, {0x15098, 0x02002021},
        {0x1a7e0, 0xae000008}, {0x1a7e4, 0xae000000}, {0x1a7e8, 0xae000004},
    };
    unsigned i;
    if (!text || size < 0x1c708 || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    return le32(text + 0x1540c) == (0x0c000000u | (((base + 0x1aab0) >> 2) & 0x03ffffffu)) &&
           le32(text + 0x151dc) == (0x0c000000u | (((base + 0x153fc) >> 2) & 0x03ffffffu)) &&
           le32(text + 0x152d8) == (0x0c000000u | (((base + 0x14f64) >> 2) & 0x03ffffffu)) &&
           le32(text + 0x15094) == (0x0c000000u | (((base + 0x1a7a8) >> 2) & 0x03ffffffu)) &&
           !memcmp(text + 0x1c6fc, "msg_bt_hid", sizeof("msg_bt_hid"));
}

int gobt_hid_route_supported(uint32_t firmware, const uint8_t *text,
                            size_t size, uint32_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        {0x14e0c, 0x27bdffe0}, {0x14e14, 0x24020003},
        {0x14e2c, 0x8e110004}, {0x14e30, 0x1222002f},
        {0x14e38, 0x94a20048}, {0x14e40, 0x3042000c},
        {0x14eb8, 0x1622fff8}, {0x14ebc, 0x24040004},
        {0x15124, 0xae320004}, {0x15128, 0x02202021},
        {0x15130, 0x03a02821},
        {0x14f64, 0x27bdffe0}, {0x14f68, 0x2ca20009},
        {0x14f80, 0x00051080}, {0x14f88, 0x00431021},
        {0x14f8c, 0x8c440000}, {0x14f90, 0x00800008},
        {0x14fd4, 0x24040004}, {0x14fe0, 0xac430000},
        {0x14fe4, 0x2604001c}, {0x14fe8, 0x27a50008},
        {0x14ff0, 0xafa20008},
        {0x15514, 0x27bdfff0}, {0x15518, 0x00a02021},
        {0x15524, 0x00002821}, {0x15534, 0x27bdfff0},
        {0x15540, 0x00a02021},
    };
    unsigned i;
    uint32_t call;
    if (firmware != 0x06060010 && firmware != 0x06060110) return 0;
    if (!text || size < 0x20c40 || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    call = 0x0c000000u | (((base + GOBT_ROUTE_NATIVE_OFFSET) >> 2) & 0x03ffffffu);
    if (le32(text + GOBT_ROUTE_CALL_OFFSET) != call) return 0;
    /* Verify method-kind 2's factory entry and exact HID vtable. These are
     * shared HID transport operations, independent of the profile's type. */
    if (le32(text + 0x1ca54) != base + 0x14fd0 ||
        le32(text + 0x14fd0) != (0x0c000000u | (((base + 0x1abe0) >> 2) & 0x03ffffffu)) ||
        le32(text + 0x14fec) != (0x08000000u | (((base + 0x14fb8) >> 2) & 0x03ffffffu))) return 0;
    {
        uint32_t table = base + 0x20c28, jump_table = base + 0x1ca4c;
        if (le32(text + 0x14f7c) != (0x3c030000u | (((jump_table + 0x8000u) >> 16) & 0xffffu)) ||
            le32(text + 0x14f84) != (0x24630000u | (jump_table & 0xffffu)) ||
            le32(text + 0x14fd8) != (0x3c030000u | (((table + 0x8000u) >> 16) & 0xffffu)) ||
            le32(text + 0x14fdc) != (0x24630000u | (table & 0xffffu))) return 0;
    }
    return le32(text + 0x20c28) == base + 0x1a8f4 &&
           le32(text + 0x20c2c) == base + 0x1a904 &&
           le32(text + 0x20c30) == base + 0x15514 &&
           le32(text + 0x20c34) == base + 0x15534 &&
           le32(text + 0x20c38) == 0 && le32(text + 0x20c3c) == 0;
}

int gobt_build_route_code(uint32_t code[GOBT_ROUTE_CODE_WORDS], uint32_t address,
                          uint32_t syscall_stub, uint32_t native_function,
                          uint32_t method_factory)
{
    uint32_t words[GOBT_ROUTE_CODE_WORDS] = {
        0x27bdffe0, /* addiu sp,sp,-32 */
        0xafbf001c, /* sw ra,28(sp) */
        0xafa40018, /* sw a0,24(sp) */
        0xafa50014, /* sw a1,20(sp) */
        0,          /* jal syscall_stub */
        0,          /* nop: delay slot */
        0x10400009, /* beqz v0,original (word 16) */
        0x8fa40018, /* lw a0,24(sp), delay slot */
        0x24050002, /* li a1,2: native generic HID method kind */
        0,          /* jal method_factory */
        0,          /* nop */
        0x24020009, /* li v0,9: plugin HID controller mode */
        0x8fbf001c, /* lw ra,28(sp) */
        0x27bd0020, /* addiu sp,sp,32 */
        0x03e00008, /* jr ra */
        0,          /* nop */
        0x8fa50014, /* original: lw a1,20(sp) */
        0x8fbf001c, /* lw ra,28(sp) */
        0x27bd0020, /* addiu sp,sp,32 */
        0,          /* j native_function, preserving restored ra */
        0,          /* nop */
    };
    if (!code || !address || !syscall_stub || !native_function || !method_factory ||
        ((address | syscall_stub | native_function | method_factory) & 3)) return -1;
    if (((address + 20) ^ syscall_stub) & 0xf0000000u) return -2;
    if (((address + 40) ^ method_factory) & 0xf0000000u) return -2;
    if (((address + 80) ^ native_function) & 0xf0000000u) return -2;
    words[4] = 0x0c000000u | ((syscall_stub >> 2) & 0x03ffffffu);
    words[9] = 0x0c000000u | ((method_factory >> 2) & 0x03ffffffu);
    words[19] = 0x08000000u | ((native_function >> 2) & 0x03ffffffu);
    memcpy(code, words, sizeof(words));
    return 0;
}

static int same_region(uint32_t address, uint32_t target)
{ return target && !(target & 3) && !((address ^ target) & 0xf0000000u); }

int gobt_build_stored_hid_code(uint32_t code[GOBT_STORED_CODE_WORDS],
                              uint32_t address, uint32_t decision, uint32_t base)
{
    uint32_t words[GOBT_STORED_CODE_WORDS] = {
        0x27bdffe0, 0xafbf001c, 0xafa40018, 0xafa50014,
        0, 0,                       /* original metadata helper */
        0xafa20010, 0x8fa50018,       /* save result; a1 = record */
        0x04410002, 0x02002021,       /* bgez v0,decision; a0 = caller's s0 profile */
        0x00002821,                  /* negative result: no record to inspect */
        0, 0x8fa60014,               /* decision(profile, record or NULL, slot) */
        0x1040000b, 0x02002021,       /* unchanged -> restore */
        0x8fa20014, 0xae020000,       /* selected slot, including a fresh saved object */
        0, 0,                       /* selected: destroy old methods/vector */
        0x02002021, 0x24050002,       /* attach ordinary HID methods */
        0, 0,
        0x24020009, 0xae020004,       /* retain custom mode after construction */
        0x8fa20010, 0x8fbf001c,       /* preserve original metadata result */
        0x03e00008, 0x27bd0020,
    };
    if (!code || !address || ((address | base) & 3)) return -1;
    if (!same_region(address + 20, base + 0x153fc) ||
        !same_region(address + 48, decision) || !same_region(address + 72, base + 0x15018) ||
        !same_region(address + 88, base + GOBT_ROUTE_FACTORY_OFFSET)) return -2;
    words[4] = 0x0c000000u | (((base + 0x153fc) >> 2) & 0x03ffffffu);
    words[11] = 0x0c000000u | ((decision >> 2) & 0x03ffffffu);
    words[17] = 0x0c000000u | (((base + 0x15018) >> 2) & 0x03ffffffu);
    words[21] = 0x0c000000u | (((base + GOBT_ROUTE_FACTORY_OFFSET) >> 2) & 0x03ffffffu);
    memcpy(code, words, sizeof(words));
    return 0;
}

int gobt_build_hid_label_code(uint32_t code[GOBT_LABEL_CODE_WORDS], uint32_t address, uint32_t base)
{
    uint32_t words[GOBT_LABEL_CODE_WORDS] = {
        0x8c820004, 0x24030009, 0x14430005, 0,
        0x00a02021, 0, 0, 0,       /* custom: tail-call translation of HID key */
        0x27bdfff0, 0xafbf0000,    /* original prologue */
        0, 0,                     /* jump back to original body */
    };
    uint32_t key = base + 0x1c6fc;
    if (!code || !address || ((address | base) & 3)) return -1;
    if (!same_region(address + 28, base + 0x15f00) ||
        !same_region(address + 44, base + GOBT_LABEL_ENTRY_OFFSET + 8)) return -2;
    words[5] = 0x3c050000u | (((key + 0x8000u) >> 16) & 0xffffu);
    words[6] = 0x08000000u | (((base + 0x15f00) >> 2) & 0x03ffffffu);
    words[7] = 0x24a50000u | (key & 0xffffu);
    words[10] = 0x08000000u | (((base + GOBT_LABEL_ENTRY_OFFSET + 8) >> 2) & 0x03ffffffu);
    memcpy(code, words, sizeof(words));
    return 0;
}

int gobt_build_hid_store_code(uint32_t code[GOBT_STORE_CODE_WORDS], uint32_t address, uint32_t base)
{
    uint32_t words[GOBT_STORE_CODE_WORDS] = {
        0x24020009, 0x14c20002, 0, 0x24060002, /* serialize mode 9 as native HID 2 */
        0x27bdff60, 0xafb40090, 0, 0,         /* original prologue and body */
    };
    if (!code || !address || ((address | base) & 3)) return -1;
    if (!same_region(address + 28, base + GOBT_STORE_ENTRY_OFFSET + 8)) return -2;
    words[6] = 0x08000000u | (((base + GOBT_STORE_ENTRY_OFFSET + 8) >> 2) & 0x03ffffffu);
    memcpy(code, words, sizeof(words));
    return 0;
}

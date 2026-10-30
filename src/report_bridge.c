#include "report_bridge.h"
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int gobt_input_metadata(const uint8_t *info, size_t size)
{
    static const char prefix[] = "DualSense";
    unsigned i;
    if (!info || size < 80 || le32(info) != 80 || info[72] != 2 || info[73] != 4)
        return 0;
    for (i = 0; i < sizeof(prefix) - 1; ++i)
        if (info[6 + 2 * i] != (uint8_t)prefix[i] || info[7 + 2 * i]) return 0;
    return !info[25] && (!info[24] || info[24] == ' ');
}

int gobt_input_identity(const uint8_t *info, size_t size, unsigned vendor, unsigned product)
{
    return vendor == 0x054c && (product == 0x0ce6 || product == 0x0df2) &&
           gobt_input_metadata(info, size);
}

int gobt_input_identity_policy(const uint8_t *info, size_t size, uint32_t identity_result,
                                unsigned vendor, unsigned product, unsigned link_flags)
{
    if (!(link_flags & 2)) return 0;
    if (!(identity_result & 0x80000000u)) return gobt_input_identity(info, size, vendor, product);
    if (identity_result == 0x802f0153u && gobt_input_metadata(info, size)) return 2;
    return 0;
}

int gobt_decode_hidp(GoBtReportAssembly *assembly, const uint8_t *packet,
                     size_t size, DsState *state)
{
    int result;
    if (!assembly) return -1;
    if (!packet || !state || size < 2 || size > 79) { assembly->used = 0; return -1; }
    /* A 79-byte HIDP frame can arrive as USB reads of 64 then 15 bytes.
     * Accept the exact remainder only when its complete report CRC verifies. */
    if (assembly->used == 63 && size == 15) {
        memcpy(assembly->report + 63, packet, 15);
        assembly->used = 0;
        result = ds_parse_bt(assembly->report, DS_BT_REPORT_SIZE, state);
        return result == 0 ? 1 : result;
    }
    if (packet[0] == 0xa1) {
        assembly->used = 0;
        if (packet[1] == 1) return ds_parse_bt_simple(packet + 1, size - 1, state) ? -1 : 1;
        if (packet[1] != 0x31) return -1;
        /* The wide receive path normally supplies a complete report. Parse
         * that immutable queued frame directly; assembly is only for actual
         * fragments, avoiding an extra 78-byte copy on every input report. */
        if (size == DS_BT_REPORT_SIZE + 1) {
            result = ds_parse_bt(packet + 1, DS_BT_REPORT_SIZE, state);
            return result == 0 ? 1 : result;
        }
    } else if (packet[0] != 0xb1 || !assembly->used) {
        assembly->used = 0;
        return -1;
    }
    if (size - 1 > DS_BT_REPORT_SIZE - assembly->used) {
        assembly->used = 0;
        return -1;
    }
    memcpy(assembly->report + assembly->used, packet + 1, size - 1);
    assembly->used += (unsigned)size - 1;
    if (assembly->used < DS_BT_REPORT_SIZE) return 0;
    assembly->used = 0;
    result = ds_parse_bt(assembly->report, DS_BT_REPORT_SIZE, state);
    return result == 0 ? 1 : result;
}

int gobt_input_setup_supported(const uint8_t *text, size_t size, uint32_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        /* Current HID connection flags, taking a device slot. */
        {0x7e4, 0x000430c0}, {0x7ec, 0x00c51021},
        /* Native HID context/read setup, taking the same slot. */
        {0xa128, 0x27bdffe0}, {0xa12c, 0xafbf001c},
        {0xa130, 0xafb20018}, {0xa134, 0xafb10014},
        {0xa138, 0x00808821}, {0xa140, 0xafb00010},
        {0xa198, 0x32440400}, {0xa1ac, 0xa6000006},
        {0xa284, 0x02202021},
    };
    unsigned i;
    uint32_t state = base + 0xdd00u;
    if (!text || size < 0xa2c0 || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    return le32(text + 0x7e0) == (0x3c030000u | (((state + 0x8000u) >> 16) & 0xffffu)) &&
           le32(text + 0x7e8) == (0x24650000u | (state & 0xffffu)) &&
           le32(text + GOBT_SETUP_FLAGS_CALL_OFFSET) ==
               (0x0c000000u | (((base + 0x7e0u) >> 2) & 0x03ffffffu));
}

int gobt_hid_reset_supported(const uint8_t *text, size_t size, uint32_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        {0xa2dc, 0x27bdffe0}, {0xa2e0, 0xafb3000c},
        {0xa300, 0x00808021}, {0xa30c, 0x3214ffff},
        {0xa31c, 0x24c50008}, {0xa328, 0x9507000c},
        {0xa32c, 0x50f40005}, {0xa330, 0x00659021},
        {0xa358, 0x26440008}, {0xa374, 0x8c440008},
        {0xa388, 0x26440020}, {0xa398, 0x26440018},
        {0xa39c, 0xa6400004}, {0xa3a8, 0xae400000},
    };
    static const struct { size_t offset; uint32_t target; } calls[] = {
        {0xa354, 0xcbe8}, /* unregister this local HID service descriptor */
        {0xa370, 0xcc88}, /* cancel this local CDC read channel */
        {0xa38c, 0xa3e4}, {0xa394, 0xa3e4}, /* release native queues */
    };
    uint32_t state = base + 0xde70u;
    unsigned i;
    if (!text || size < 0xa3ac || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i)
        if (le32(text + calls[i].offset) !=
            (0x0c000000u | (((base + calls[i].target) >> 2) & 0x03ffffffu))) return 0;
    /* This helper touches local input resources. It sends no Bluetooth
     * disconnect or pairing commands; that separate helper is at 0x2b10. */
    return le32(text + 0xa2e4) == (0x3c130000u | (((state + 0x8000u) >> 16) & 0xffffu)) &&
           le32(text + 0xa310) == (0x26660000u | (state & 0xffffu));
}

int gobt_hid_close_supported(const uint8_t *text, size_t size, uint32_t base)
{
    uint32_t table = base + 0xdab8u;
    if (!text || size < GOBT_HID_CLOSE_POINTER_OFFSET + 4 || (base & 3)) return 0;
    /* The manager clears profile flags before dispatching this table. Its
     * jalr passes the device slot and ignores the cleanup return value. */
    return le32(text + GOBT_HID_CLOSE_POINTER_OFFSET) == base + 0xa2dcu &&
           le32(text + 0x26cc) == (0x3c030000u | (((table + 0x8000u) >> 16) & 0xffffu)) &&
           le32(text + 0x26d0) == (0x246c0000u | (table & 0xffffu)) &&
           le32(text + 0x26d4) == 0x00118880u &&
           le32(text + 0x26dc) == 0x8d620000u &&
           le32(text + 0x2718) == 0x0040f809u &&
           le32(text + 0x271c) == 0x96440000u &&
           le32(text + 0x2720) == (0x08000000u | (((base + 0x26ecu) >> 2) & 0x03ffffffu));
}

int gobt_session_event_supported(const uint8_t *text, size_t size, uint32_t base)
{
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0xa28, 0x27a40004}, {0x2044, 0x27bdffe0},
        {0x2058, 0x244300e0}, {0x2070, 0x94640004},
        {0x2074, 0x50850038}, {0x2078, 0x90620006},
        {0x2158, 0x92060004}, {0x216c, 0x96110002},
        {0x2190, 0x146affc1}, {0x21a4, 0x8f0400e8},
        {0x21b8, 0x03a03821}, {0x21c8, 0x30c40008},
        {0x21cc, 0x50800009}, {0x21e4, 0x02002021},
        {0x2214, 0x8e2400e8}, {0x2224, 0x2405fff7},
        {0x2230, 0x24050004},
        {0x255c, 0x27bdffe0}, {0x2560, 0xafb3000c},
        {0x2564, 0x00a09821}, {0x256c, 0x00809021},
        {0x25b8, 0x924c0004}, {0x25c8, 0xa5090004},
        {0x2628, 0xa6600002}, {0x262c, 0xa6600000},
    };
    unsigned i;
    uint32_t state = base + 0xdd00u;
    if (!text || size < 0x2728 || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    return le32(text + GOBT_SESSION_EVENT_CALL_OFFSET) ==
               (0x0c000000u | (((base + 0x2044u) >> 2) & 0x03ffffffu)) &&
           le32(text + 0x21e0) == (0x0c000000u | (((base + 0x255cu) >> 2) & 0x03ffffffu)) &&
           le32(text + 0x204c) == (0x3c120000u | (((state + 0x8000u) >> 16) & 0xffffu)) &&
           le32(text + 0x2050) == (0x26420000u | (state & 0xffffu));
}

int gobt_connect_epoch_supported(const uint8_t *text, size_t size, uint32_t base)
{
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0x2728, 0x27bdff90}, {0x2738, 0x00809821}, /* s3 = device */
        {0x2828, 0x244600e4}, {0x2854, 0x8e030004},
        {0x285c, 0xa6110002}, /* s0 = reserved active entry */
        {0x2874, 0x8e040004}, {0x2878, 0x2405ffff}, {0x287c, 0x24060001},
        {0x2884, 0x27a70040}, {0x2888, 0x04400033},
        {0x2894, 0x318b0008}, {0x2898, 0x1560002f},
        {0x29ec, 0x24050005}, {0x29f4, 0x27a70044}, {0x29f8, 0x0280f809},
        {0x2a0c, 0x32840004}, {0x2a10, 0x1080ffd0}, {0x2a2c, 0x36b20140},
    };
    unsigned i;
    if (!text || size < 0x2a30 || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    return le32(text + 0x2a20) == (0x0c000000u | (((base + 0xcb20u) >> 2) & 0x03ffffffu)) &&
           le32(text + GOBT_CONNECT_POLL_CALL_OFFSET) ==
           (0x0c000000u | (((base + 0xcab8u) >> 2) & 0x03ffffffu));
}

int gobt_cached_record_supported(const uint8_t *text, size_t size, uint32_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        /* Common serialized command helper and the native cached-record ABI. */
        {0x3720, 0x27bdffc0}, {0x3724, 0xafb10024}, {0x372c, 0xafb50034},
        {0x3738, 0x00809821}, {0x3744, 0x00c09021}, {0x3750, 0x00a08021},
        {0x43ac, 0x240c0063}, {0x43b0, 0x240b0002},
        {0x43b4, 0x240a0163}, {0x43b8, 0x24090050},
        {0x43c4, 0x02403021}, {0x43c8, 0xa7b10000},
        {0x43cc, 0xa7ac0010}, {0x43d0, 0xa7ab0012},
        {0x43d4, 0xa7aa0020}, {0x43d8, 0xa7a90022},
        {0x43dc, 0xafb00028}, {0x43e4, 0xafbd0014},
        /* Paired-slot table: ID/capabilities at +4/+6 and address at +10
         * relative to state + 0x10 + slot*16. Used for known-target startup. */
        {0x436c, 0x00043900}, {0x4378, 0x00f31821}, {0x437c, 0x24620010},
        {0x4394, 0x94470004}, {0x43a0, 0x94450006},
        {0x1524, 0x24c7002a}, {0x1538, 0x24d00024},
        {0x15ac, 0xa5090006}, {0x15b0, 0xa5060004},
        {0x15b4, 0xa2220005}, {0x15b8, 0xa2390000},
        {0x15bc, 0xa2380001}, {0x15c0, 0xa2350002},
        {0x15c4, 0xa2340003}, {0x15c8, 0xa22f0004},
    };
    uint32_t state = base + 0xdd00u;
    unsigned i;
    if (!text || size < 0x43e8 || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    return le32(text + 0x3728) == (0x3c110000u | (((state + 0x8000u) >> 16) & 0xffffu)) &&
           le32(text + 0x3730) == (0x26350000u | (state & 0xffffu)) &&
           le32(text + 0x4364) == (0x3c020000u | (((state + 0x8000u) >> 16) & 0xffffu)) &&
           le32(text + 0x4370) == (0x24530000u | (state & 0xffffu)) &&
           le32(text + 0x43e0) == (0x0c000000u | (((base + 0x3720u) >> 2) & 0x03ffffffu));
}

int gobt_cdc_read_state_supported(const uint8_t *text, size_t size, uint32_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        {0x1588, 0x00114840}, {0x158c, 0x01314021}, {0x1590, 0x00083080},
        {0x15a0, 0x8c43007c}, {0x15a8, 0xac530084},
        {0x1634, 0xac54007c}, {0x163c, 0xac500080},
    };
    unsigned i;
    uint32_t state = base + 0x23a0u;
    if (!text || size < 0x1640 || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    return le32(text + 0x1574) == (0x3c160000u | (((state + 0x8000u) >> 16) & 0xffffu)) &&
           le32(text + 0x1594) == (0x26c70000u | (state & 0xffffu));
}

int gobt_input_discovery_supported(const uint8_t *text, size_t size, uint32_t base)
{
    if (!text || size < 0xcc00 || (base & 3)) return 0;
    /* Native DID helper and HID-service registration call, including delay
     * slots and the success/failure branch. Only these JAL targets change. */
    return le32(text + 0xa1e4) == (0x0c000000u | (((base + 0x58d8u) >> 2) & 0x03ffffffu)) &&
           le32(text + 0xa1e8) == 0x00003821u &&
           le32(text + 0xa1fc) == (0x0c000000u | (((base + 0xcbf8u) >> 2) & 0x03ffffffu)) &&
           le32(text + 0xa200) == 0x26040008u &&
           le32(text + 0xa204) == 0x04400018u &&
           le32(text + 0xa208) == 0x00408821u;
}

int gobt_reconnect_supported(const uint8_t *text, size_t size, uint32_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        {0x18d4, 0x96040004},
        {0x202c, 0x8e24000c}, {0x2030, 0x0443ff18}, {0x2034, 0x8e240008},
        {0x2038, 0x8e24000c}, {0x2040, 0x24050002},
        {0x1dcc, 0x00003021}, {0x2b10, 0x27bdff90},
        {0x2b18, 0xafb40060}, {0x2b1c, 0x00a0a021},
    };
    unsigned i;
    uint32_t call = 0x0c000000u | (((base + 0xa128u) >> 2) & 0x03ffffffu);
    uint32_t state = base + 0xcac0u;
    if (!text || size < 0x2b30 || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    return le32(text + GOBT_SETUP_EVENT_CALL_OFFSET) == call &&
           le32(text + GOBT_SETUP_CHECKED_CALL_OFFSET) == call &&
           le32(text + 0x18d8) == (0x08000000u | (((base + 0x1708u) >> 2) & 0x03ffffffu)) &&
           le32(text + 0x203c) == (0x08000000u | (((base + 0x1dc8u) >> 2) & 0x03ffffffu)) &&
           le32(text + 0x1dc8) == (0x0c000000u | (((base + 0x2b10u) >> 2) & 0x03ffffffu)) &&
           le32(text + 0x2b14) == (0x3c020000u | (((state + 0x8000u) >> 16) & 0xffffu)) &&
           le32(text + 0x2b2c) == (0x24520000u | (state & 0xffffu));
}

int gobt_report_bridge_supported(uint32_t firmware, const uint8_t *text,
                                  size_t size, uint32_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        {0x9ee0, 0x27bdffd0}, {0x9ee4, 0xafb3001c}, {0x9ee8, 0x00c09821},
        {0x9efc, 0x8cd20028}, {0x9f00, 0xacc00028}, {0x9f08, 0xae450004},
        {0x9f30, 0xae630028}, {0x9f34, 0x24060040},
        {0x9f38, 0x02604021}, {0x9f3c, 0x8c650008}, {0x9f44, 0x8e640000},
        {0xa220, 0x8e250008}, {0xa228, 0x02004021}, {0xa230, 0x24060040},
    };
    unsigned i;
    uint32_t call, callback = base + GOBT_RX_CALLBACK_OFFSET;
    if (firmware != 0x06060010 && firmware != 0x06060110) return 0;
    if (!text || size < 0xccc8 || (base & 3)) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (le32(text + sites[i].offset) != sites[i].word) return 0;
    if (le32(text + 0xa21c) != (0x3c0f0000u | (((callback + 0x8000u) >> 16) & 0xffffu)) ||
        le32(text + 0xa224) != (0x25e70000u | (callback & 0xffffu))) return 0;
    call = 0x0c000000u | (((base + GOBT_RX_SUBMIT_OFFSET) >> 2) & 0x03ffffffu);
    /* Verify the address used to build the two fixed native contexts too. */
    if (le32(text + 0xa8fc) != (0x3c030000u | (((base + 0xde70u + 0x8000u) >> 16) & 0xffffu)) ||
        le32(text + 0xa900) != (0x24620000u | ((base + 0xde70u) & 0xffffu))) return 0;
    return le32(text + GOBT_RX_FIRST_CALL_OFFSET) == call &&
           le32(text + GOBT_RX_NEXT_CALL_OFFSET) == call;
}

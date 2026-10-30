#include "probe_policy.h"
#include <string.h>

int gobt_capture_module(const char *name)
{
    char lower[28];
    unsigned i;
    if (!name || !strcmp(name, "PSPGoBT_Probe")) return 0;
    for (i = 0; i < sizeof(lower) - 1 && name[i]; ++i) {
        char ch = name[i];
        lower[i] = ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
    }
    lower[i] = 0;
    return strstr(lower, "bluetooth") || strstr(lower, "netconf_bt") ||
           strstr(lower, "bsman") || strstr(lower, "bsmcdc") ||
           strstr(lower, "usbhost") || strstr(lower, "usb_host") ||
           strstr(lower, "btdun") || strstr(lower, "hidsvc") ||
           strstr(lower, "padsvc") || strstr(lower, "input") ||
           strstr(lower, "controller") || strstr(lower, "sysconf");
}

int gobt_device_info_trace_supported(uint32_t firmware, const uint8_t *text, size_t size)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        {0x150b0, 0x27bdffa0}, /* addiu sp,sp,-96 */
        {0x150ec, 0x24020007}, /* li v0,7 */
        {0x150f0, 0x12420004}, /* beq s2,v0,accepted */
        {0x150f4, 0x3c10802f}, /* lui s0,0x802f */
        {0x150f8, 0x97a20048}, /* lhu v0,72(sp) */
        {0x150fc, 0x1040000e}, /* beqz v0,return */
        {0x15100, 0x36100135}, /* ori s0,s0,0x135 */
        {0x153f0, 0x24020050}, /* device-record constructor: size=80 */
        {0x153f4, 0x03e00008},
        {0x153f8, 0xac820000},
    };
    unsigned i;
    if (firmware != 0x06060010 && firmware != 0x06060110) return 0;
    if (!text || size < 0x15400) return 0;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        uint32_t word;
        memcpy(&word, text + sites[i].offset, sizeof(word));
        if (word != sites[i].word) return 0;
    }
    return 1;
}

int gobt_bsman_api_supported(uint32_t nid, const uint8_t *code, size_t size)
{
    const uint32_t *expected = NULL;
    size_t offset = 0, count = 4, i;
    static const uint32_t info[] = {0x27bdff90};
    static const uint32_t scan[] = {0x27bdfff0, 0x24860054, 0x00c41025, 0xafb00000};
    static const uint32_t address[] = {0x24870001, 0x001b12c0, 0x00e43025, 0x27bdffa0};
    static const uint32_t connect[] = {0x27bdfff0, 0x00a01021, 0x24060402, 0x24050202};
    static const uint32_t identity[] = {0x2483ffff, 0x27bdfff0, 0x2cc20001, 0x2c6a0008};
    static const uint32_t disconnect[] = {0x27bdfff0, 0xafb00000, 0x24050002, 0x03608021};
    switch (nid) {
    case 0xcbd232d1: expected = info; offset = 8; count = 1; break;
    case 0x8ac2f7b8: expected = scan; break;
    case 0x2d649923: expected = address; break;
    case 0xf188a405: expected = connect; break;
    case 0x1238f36e: expected = identity; break;
    case 0x68327a27: expected = disconnect; break;
    default: return 0;
    }
    if (!code || size < offset + count * 4) return 0;
    for (i = 0; i < count; ++i) {
        uint32_t word;
        memcpy(&word, code + offset + 4 * i, sizeof(word));
        if (word != expected[i]) return 0;
    }
    return 1;
}

#include "target_cache.h"
#include "dualsense.h"
#include <string.h>

static int valid_address(const uint8_t *address)
{
    unsigned i, any = 0, all = 255;
    if (!address) return 0;
    for (i = 0; i < 6; ++i) { any |= address[i]; all &= address[i]; }
    return any && all != 255;
}

int gobt_target_contains(const GoBtTargetCache *cache, const uint8_t address[6])
{
    unsigned i;
    if (!cache || cache->count > 2 || !valid_address(address)) return 0;
    for (i = 0; i < cache->count; ++i)
        if (!memcmp(cache->address[i], address, 6)) return 1;
    return 0;
}

int gobt_target_remember(GoBtTargetCache *cache, const uint8_t address[6])
{
    unsigned index;
    if (!cache || cache->count > 2 || !valid_address(address)) return -1;
    if (gobt_target_contains(cache, address)) return 0;
    if (cache->count < 2) index = cache->count++;
    else { memcpy(cache->address[0], cache->address[1], 6); index = 1; }
    memcpy(cache->address[index], address, 6);
    return 1;
}

int gobt_target_encode(const GoBtTargetCache *cache, uint8_t bytes[GOBT_TARGET_CACHE_BYTES])
{
    unsigned i;
    uint32_t crc;
    if (!cache || !bytes || cache->count > 2) return -1;
    for (i = 0; i < cache->count; ++i)
        if (!valid_address(cache->address[i])) return -1;
    if (cache->count == 2 && !memcmp(cache->address[0], cache->address[1], 6)) return -1;
    memset(bytes, 0, GOBT_TARGET_CACHE_BYTES);
    memcpy(bytes, "GBTC", 4);
    bytes[4] = 1;
    bytes[5] = (uint8_t)cache->count;
    memcpy(bytes + 8, cache->address, cache->count * 6);
    crc = ds_crc32(0, bytes, 28);
    for (i = 0; i < 4; ++i) bytes[28 + i] = (uint8_t)(crc >> (8 * i));
    return 0;
}

int gobt_target_decode(GoBtTargetCache *cache, const uint8_t *bytes, size_t size)
{
    unsigned i;
    uint32_t crc = 0;
    if (!cache) return -1;
    memset(cache, 0, sizeof(*cache));
    if (!bytes || size != GOBT_TARGET_CACHE_BYTES || memcmp(bytes, "GBTC", 4) ||
        bytes[4] != 1 || bytes[5] > 2 || bytes[6] || bytes[7]) return -1;
    for (i = 8 + bytes[5] * 6; i < 28; ++i) if (bytes[i]) return -1;
    for (i = 0; i < 4; ++i) crc |= (uint32_t)bytes[28 + i] << (8 * i);
    if (crc != ds_crc32(0, bytes, 28)) return -1;
    for (i = 0; i < bytes[5]; ++i)
        if (gobt_target_remember(cache, bytes + 8 + 6 * i) != 1) {
            memset(cache, 0, sizeof(*cache));
            return -1;
        }
    return 0;
}

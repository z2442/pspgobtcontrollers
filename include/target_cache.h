#ifndef PSPGOBT_TARGET_CACHE_H
#define PSPGOBT_TARGET_CACHE_H
#include <stddef.h>
#include <stdint.h>

#define GOBT_TARGET_CACHE_BYTES 32u
typedef struct {
    unsigned count;
    uint8_t address[2][6];
} GoBtTargetCache;

int gobt_target_contains(const GoBtTargetCache *cache, const uint8_t address[6]);
int gobt_target_remember(GoBtTargetCache *cache, const uint8_t address[6]);
int gobt_target_encode(const GoBtTargetCache *cache, uint8_t bytes[GOBT_TARGET_CACHE_BYTES]);
int gobt_target_decode(GoBtTargetCache *cache, const uint8_t *bytes, size_t size);
#endif

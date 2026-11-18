#ifndef PSPGOBT_MAPPING_UI_H
#define PSPGOBT_MAPPING_UI_H
#include "controller_map.h"
#include <stdint.h>

#define GOBT_MAPPING_ABI 1u
/* 4: generic-path bypass; the editor opens only through native Triangle. */
#define GOBT_MAPPING_UI_ABI 4u
#define GOBT_MAPPING_GET 1u
#define GOBT_MAPPING_SAVE 2u
#define GOBT_MAPPING_FLUSH 3u
#define GOBT_MAPPING_PATCH 4u
/* Four words {stage, a, b, c}: helper progress for session-trace builds. */
#define GOBT_MAPPING_TRACE 5u
typedef struct {
    uint32_t abi, device;
    uint8_t address[6], reserved[2];
    GoBtMap map;
} GoBtMappingRequest;
typedef struct {
    uint32_t abi, base, size, gateway;
} GoBtMappingUiArgs;
typedef struct {
    uint32_t abi, base, size, append, bypass;
} GoBtMappingPatch;

void gobt_mapping_select(const uint8_t address[6]);
void gobt_mapping_apply(const DsState *source, DsState *destination);
#endif

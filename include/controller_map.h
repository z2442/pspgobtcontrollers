#ifndef PSPGOBT_CONTROLLER_MAP_H
#define PSPGOBT_CONTROLLER_MAP_H
#include "dualsense.h"

#define GOBT_MAP_BUTTONS 17
#define GOBT_MAP_OFF 255
#define GOBT_MAP_RECORDS 8
#define GOBT_MAP_FILE_BYTES 272

/* Physical, normalized source button -> normalized destination button.
 * PSP L accepts L1/L2 and PSP R accepts R1/R2. Stick: 0 left, 1 right,
 * 2 disabled. Flags bit 0/1 invert X/Y. Reserved bytes must remain zero. */
typedef struct {
    uint8_t button[GOBT_MAP_BUTTONS];
    uint8_t stick, flags, reserved[5];
} GoBtMap;
typedef struct {
    uint8_t address[6];
    GoBtMap map;
} GoBtMapRecord;
typedef struct {
    unsigned count;
    GoBtMapRecord record[GOBT_MAP_RECORDS];
} GoBtMapBook;

void gobt_map_default(GoBtMap *map);
int gobt_map_valid(const GoBtMap *map);
void gobt_map_apply(const GoBtMap *map, const DsState *source, DsState *destination);
int gobt_map_find(const GoBtMapBook *book, const uint8_t address[6]);
int gobt_map_put(GoBtMapBook *book, const uint8_t address[6], const GoBtMap *map);
int gobt_map_encode(const GoBtMapBook *book, uint8_t bytes[GOBT_MAP_FILE_BYTES]);
int gobt_map_decode(GoBtMapBook *book, const uint8_t *bytes, size_t size);
#endif

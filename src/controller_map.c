#include "controller_map.h"
#include <string.h>

static int address_valid(const uint8_t *address)
{
    unsigned i, any = 0, all = 255;
    if (!address) return 0;
    for (i = 0; i < 6; ++i) { any |= address[i]; all &= address[i]; }
    return any && all != 255;
}

void gobt_map_default(GoBtMap *map)
{
    unsigned i;
    memset(map, 0, sizeof(*map));
    for (i = 0; i < GOBT_MAP_BUTTONS; ++i) map->button[i] = i;
}

int gobt_map_valid(const GoBtMap *map)
{
    unsigned i;
    if (!map || map->stick > 2 || map->flags > 3) return 0;
    for (i = 0; i < GOBT_MAP_BUTTONS; ++i)
        if (map->button[i] >= GOBT_MAP_BUTTONS && map->button[i] != GOBT_MAP_OFF) return 0;
    for (i = 0; i < sizeof(map->reserved); ++i)
        if (map->reserved[i]) return 0;
    return 1;
}

static uint8_t invert(uint8_t value)
{
    /* Preserve the neutral center and both extrema. */
    return value == 0 ? 255 : value == 255 ? 0 : 256 - value;
}

void gobt_map_apply(const GoBtMap *map, const DsState *source, DsState *destination)
{
    unsigned i;
    *destination = *source;
    destination->buttons = 0;
    destination->l2 = destination->r2 = 0;
    for (i = 0; i < GOBT_MAP_BUTTONS; ++i) {
        unsigned target = map->button[i];
        unsigned pressure = i == 8 ? source->l2 : i == 9 ? source->r2 :
                            (source->buttons & (1u << i)) ? 255 : 0;
        if (target == GOBT_MAP_OFF) continue;
        if (source->buttons & (1u << i)) destination->buttons |= 1u << target;
        if (target == 8 && pressure > destination->l2) destination->l2 = pressure;
        if (target == 9 && pressure > destination->r2) destination->r2 = pressure;
    }
    if (map->stick == 1) { destination->lx = source->rx; destination->ly = source->ry; }
    if (map->stick == 2) destination->lx = destination->ly = 128;
    if (map->flags & 1) destination->lx = invert(destination->lx);
    if (map->flags & 2) destination->ly = invert(destination->ly);
}

int gobt_map_find(const GoBtMapBook *book, const uint8_t address[6])
{
    unsigned i;
    if (!book || book->count > GOBT_MAP_RECORDS || !address_valid(address)) return -1;
    for (i = 0; i < book->count; ++i)
        if (!memcmp(book->record[i].address, address, 6)) return i;
    return -1;
}

int gobt_map_put(GoBtMapBook *book, const uint8_t address[6], const GoBtMap *map)
{
    int index;
    if (!book || book->count > GOBT_MAP_RECORDS || !address_valid(address) || !gobt_map_valid(map)) return -1;
    index = gobt_map_find(book, address);
    if (index < 0) {
        if (book->count == GOBT_MAP_RECORDS) return -2;
        index = book->count++;
    }
    memcpy(book->record[index].address, address, 6);
    book->record[index].map = *map;
    return 0;
}

static void store32(uint8_t *bytes, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) bytes[i] = value >> (8 * i);
}

int gobt_map_encode(const GoBtMapBook *book, uint8_t bytes[GOBT_MAP_FILE_BYTES])
{
    unsigned i;
    if (!book || book->count > GOBT_MAP_RECORDS || !bytes) return -1;
    memset(bytes, 0, GOBT_MAP_FILE_BYTES);
    memcpy(bytes, "GBMP", 4);
    bytes[4] = 1;
    bytes[5] = book->count;
    for (i = 0; i < book->count; ++i) {
        if (!address_valid(book->record[i].address) || !gobt_map_valid(&book->record[i].map) ||
            gobt_map_find(book, book->record[i].address) != (int)i) return -1;
        memcpy(bytes + 8 + i * 32, book->record[i].address, 6);
        memcpy(bytes + 14 + i * 32, &book->record[i].map, sizeof(GoBtMap));
    }
    store32(bytes + GOBT_MAP_FILE_BYTES - 4, ds_crc32(0, bytes, GOBT_MAP_FILE_BYTES - 4));
    return 0;
}

int gobt_map_decode(GoBtMapBook *book, const uint8_t *bytes, size_t size)
{
    GoBtMapBook decoded;
    uint8_t canonical[GOBT_MAP_FILE_BYTES];
    unsigned i;
    if (!book || !bytes || size != GOBT_MAP_FILE_BYTES || memcmp(bytes, "GBMP", 4) ||
        bytes[4] != 1 || bytes[5] > GOBT_MAP_RECORDS) return -1;
    memset(&decoded, 0, sizeof(decoded));
    for (i = 0; i < bytes[5]; ++i) {
        GoBtMap map;
        memcpy(&map, bytes + 14 + i * 32, sizeof(map));
        if (gobt_map_find(&decoded, bytes + 8 + i * 32) >= 0 ||
            gobt_map_put(&decoded, bytes + 8 + i * 32, &map) < 0) return -1;
    }
    if (gobt_map_encode(&decoded, canonical) < 0 || memcmp(canonical, bytes, size)) return -1;
    *book = decoded;
    return 0;
}

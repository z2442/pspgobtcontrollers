#include "controller_map.h"
#include "mapping_patch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    GoBtMap map;
    GoBtMapBook book = {0}, loaded = {0}, before;
    DsState input, output;
    uint8_t bytes[GOBT_MAP_FILE_BYTES], bad[GOBT_MAP_FILE_BYTES];
    const uint8_t first[6] = {1,2,3,4,5,6}, second[6] = {6,5,4,3,2,1};
    unsigned i;
    gobt_map_default(&map);
    assert(gobt_map_valid(&map));
    /* Identity includes partial analog trigger pressures and all 17 buttons. */
    for (i = 0; i <= 0x1ffff; ++i) {
        ds_neutral(&input);
        input.buttons = i;
        input.l2 = i % 256;
        input.r2 = (i / 7) % 256;
        gobt_map_apply(&map, &input, &output);
        assert(output.buttons == input.buttons && output.l2 == input.l2 && output.r2 == input.r2);
        assert(output.lx == input.lx && output.ly == input.ly && output.rx == input.rx && output.ry == input.ry);
    }
    ds_neutral(&input);
    input.buttons = (1u << 14) | (1u << 8) | (1u << 10);
    input.l2 = 76;
    input.rx = 0; input.ry = 255;
    map.button[14] = 13; /* Cross -> Circle */
    map.button[8] = GOBT_MAP_OFF;
    map.button[10] = 9; /* L1 -> R2, with digital pressure */
    map.stick = 1; map.flags = 3;
    gobt_map_apply(&map, &input, &output);
    assert(output.buttons == ((1u << 13) | (1u << 9)));
    assert(output.l2 == 0 && output.r2 == 255);
    assert(output.lx == 255 && output.ly == 0);
    input.rx = input.ry = 128;
    gobt_map_apply(&map, &input, &output);
    assert(output.lx == 128 && output.ly == 128);
    map.stick = 2;
    gobt_map_apply(&map, &input, &output);
    assert(output.lx == 128 && output.ly == 128);
    assert(gobt_map_put(&book, first, &map) == 0);
    gobt_map_default(&map);
    assert(gobt_map_put(&book, second, &map) == 0);
    assert(gobt_map_encode(&book, bytes) == 0);
    assert(gobt_map_decode(&loaded, bytes, sizeof(bytes)) == 0);
    assert(loaded.count == 2 && loaded.record[gobt_map_find(&loaded, first)].map.button[14] == 13);
    assert(loaded.record[gobt_map_find(&loaded, second)].map.button[14] == 14);
    before = loaded;
    for (i = 0; i < sizeof(bytes); ++i) {
        memcpy(bad, bytes, sizeof(bad)); bad[i] ^= 1;
        assert(gobt_map_decode(&loaded, bad, sizeof(bad)) < 0);
        assert(!memcmp(&loaded, &before, sizeof(loaded)));
    }
    assert(gobt_map_decode(&loaded, bytes, sizeof(bytes)-1) < 0);
    map.button[4] = 17;
    assert(!gobt_map_valid(&map) && gobt_map_put(&book, first, &map) < 0);
    gobt_map_default(&map);
    for (i = 2; i < GOBT_MAP_RECORDS; ++i) {
        uint8_t address[6] = {42,0,0,0,0,0}; address[5] = i;
        assert(gobt_map_put(&book, address, &map) == 0);
    }
    {
        uint8_t address[6] = {99,0,0,0,0,0};
        assert(gobt_map_put(&book, address, &map) == -2);
    }
    puts("Controller mapping and per-address persistence checks passed");
    return 0;
}

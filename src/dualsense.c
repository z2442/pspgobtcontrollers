#include "dualsense.h"
#include <string.h>

static uint32_t crc_byte(uint32_t crc, uint8_t byte)
{
    /* Two nibble lookups replace eight bit steps; 64 bytes of read-only
     * data keeps the hot receive path small without weakening validation. */
    static const uint32_t table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
    };
    crc ^= byte;
    crc = (crc >> 4) ^ table[crc & 15];
    return (crc >> 4) ^ table[crc & 15];
}

uint32_t ds_crc32(uint8_t prefix, const uint8_t *data, size_t size)
{
    uint32_t crc = crc_byte(0xffffffffu, prefix);
    size_t i;
    for (i = 0; i < size; ++i)
        crc = crc_byte(crc, data[i]);
    return ~crc;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t n)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        p[i] = (uint8_t)(n >> (8 * i));
}

void ds_neutral(DsState *state)
{
    memset(state, 0, sizeof(*state));
    state->lx = state->ly = state->rx = state->ry = 128;
}

static int decode_controls(const uint8_t *axes, uint8_t l2, uint8_t r2,
                            uint8_t face_hat, uint8_t shoulders, uint8_t system,
                            DsState *state)
{
    static const uint8_t hats[9] = {
        0x10, 0x30, 0x20, 0x60, 0x40, 0xc0, 0x80, 0x90, 0
    };
    DsState next;
    uint8_t hat, b;
    hat = face_hat & 15;
    if (hat > 8)
        return -3;
    ds_neutral(&next);
    next.lx = axes[0]; next.ly = axes[1];
    next.rx = axes[2]; next.ry = axes[3];
    next.l2 = l2; next.r2 = r2;
    next.buttons = hats[hat];
    b = shoulders;
    if (b & 0x10) next.buttons |= 1u << 0;  /* Create -> Select */
    if (b & 0x40) next.buttons |= 1u << 1;  /* L3 */
    if (b & 0x80) next.buttons |= 1u << 2;  /* R3 */
    if (b & 0x20) next.buttons |= 1u << 3;  /* Options -> Start */
    if (b & 0x04) next.buttons |= 1u << 8;  /* L2 */
    if (b & 0x08) next.buttons |= 1u << 9;  /* R2 */
    if (b & 0x01) next.buttons |= 1u << 10; /* L1 */
    if (b & 0x02) next.buttons |= 1u << 11; /* R1 */
    b = face_hat;
    if (b & 0x80) next.buttons |= 1u << 12; /* Triangle */
    if (b & 0x40) next.buttons |= 1u << 13; /* Circle */
    if (b & 0x20) next.buttons |= 1u << 14; /* Cross */
    if (b & 0x10) next.buttons |= 1u << 15; /* Square */
    if (system & 1) next.buttons |= 1u << 16; /* PS */
    *state = next;
    return 0;
}

int ds_parse_bt(const uint8_t *report, size_t size, DsState *state)
{
    if (!report || !state || size != DS_BT_REPORT_SIZE || report[0] != 0x31)
        return -1;
    if (ds_crc32(0xa1, report, size - 4) != get_le32(report + size - 4))
        return -2;
    return decode_controls(report + 2, report[6], report[7], report[9], report[10], report[11], state);
}

int ds_parse_bt_simple(const uint8_t *report, size_t size, DsState *state)
{
    if (!report || !state || size != 10 || report[0] != 1) return -1;
    return decode_controls(report + 1, report[8], report[9], report[5], report[6], report[7], state);
}

void ds_to_ds3(const DsState *state, uint8_t report[DS3_REPORT_SIZE])
{
    static const uint8_t pressure_bits[12] = {
        4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    unsigned i;
    memset(report, 0, DS3_REPORT_SIZE);
    report[0] = 1;
    report[2] = (uint8_t)state->buttons;
    report[3] = (uint8_t)(state->buttons >> 8);
    report[4] = (uint8_t)((state->buttons >> 16) & 1);
    report[6] = state->lx; report[7] = state->ly;
    report[8] = state->rx; report[9] = state->ry;
    for (i = 0; i < 12; ++i)
        report[14 + i] = (state->buttons & (1u << pressure_bits[i])) ? 255 : 0;
    report[18] = state->l2;
    report[19] = state->r2;
    /* Neutral DS3 motion axes, big endian 10-bit readings. */
    report[41] = 2; report[43] = 2; report[45] = 2; report[47] = 2;
}

void ds_build_enable(uint8_t report[DS_BT_REPORT_SIZE], uint8_t sequence)
{
    memset(report, 0, DS_BT_REPORT_SIZE);
    report[0] = 0x31;
    report[1] = (uint8_t)((sequence & 15) << 4);
    report[2] = 0x10; /* Bluetooth output tag. */
    report[4] = 0x04; /* Lightbar control valid. */
    report[49] = 0x40; /* Dim blue; motors and adaptive triggers disabled. */
    put_le32(report + 74, ds_crc32(0xa2, report, 74));
}

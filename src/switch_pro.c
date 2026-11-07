#include "switch_pro.h"
#include <string.h>

/* Wire format references: dekuNukem/Nintendo_Switch_Reverse_Engineering,
 * bluetooth_hid_notes.md and spi_flash_notes.md. Independent implementation. */
static unsigned packed_axis(const uint8_t *p, unsigned y)
{
    return y ? (unsigned)(p[1] >> 4) | ((unsigned)p[2] << 4) :
               (unsigned)p[0] | ((unsigned)(p[1] & 15) << 8);
}

static uint8_t map_axis(const SwitchProAxis *axis, unsigned raw, int invert)
{
    unsigned center = axis->center ? axis->center : 2048;
    unsigned below = axis->below ? axis->below : 1434;
    unsigned above = axis->above ? axis->above : 1434;
    int delta = invert ? (int)center - (int)raw : (int)raw - (int)center;
    unsigned travel = (raw < center) ? below : above;
    unsigned distance = (unsigned)(delta < 0 ? -delta : delta);
    int value;
    /* A small center dead zone prevents idle drift with nominal calibration. */
    if (distance <= travel / 20) return 128;
    value = delta < 0 ? 128 - (int)(distance * 128 / travel) :
                        128 + (int)(distance * 127 / travel);
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static void calibration(SwitchPro *pro, const uint8_t *data, unsigned stick)
{
    SwitchProAxis next[2];
    unsigned i;
    /* Left: above, center, below; right: center, below, above. */
    unsigned center = stick ? 0 : 3, below = stick ? 3 : 6, above = stick ? 6 : 0;
    for (i = 0; i < 2; ++i) {
        next[i].center = (uint16_t)packed_axis(data + center, i);
        next[i].below = (uint16_t)packed_axis(data + below, i);
        next[i].above = (uint16_t)packed_axis(data + above, i);
        if (next[i].center < 512 || next[i].center > 3583 ||
            next[i].below < 256 || next[i].above < 256 ||
            next[i].below > next[i].center ||
            next[i].above > 4095 - next[i].center) return;
    }
    memcpy(pro->axis + 2 * stick, next, sizeof(next));
}

static void advance(SwitchPro *pro)
{
    ++pro->step;
    pro->attempts = pro->waiting = 0;
}

int switch_pro_setup(SwitchPro *pro, uint64_t now, uint8_t report[SWITCH_PRO_REPORT_SIZE])
{
    static const uint8_t neutral[8] = {0, 1, 0x40, 0x40, 0, 1, 0x40, 0x40};
    if (!pro || !report || now < pro->retry_after || pro->step >= SWITCH_PRO_DONE) return 0;
    if (pro->attempts >= 3) {
        /* A silent report-mode request is retried by the bridge's stream
         * recovery. Optional calibration/LED failures must not block input. */
        if (pro->step == SWITCH_PRO_MODE) return 0;
        advance(pro);
        if (pro->step == SWITCH_PRO_DONE) return 0;
    }
    memset(report, 0, SWITCH_PRO_REPORT_SIZE);
    report[0] = 1;
    report[1] = (uint8_t)(pro->sequence++ & 15);
    memcpy(report + 2, neutral, sizeof(neutral));
    if (pro->step == SWITCH_PRO_MODE) {
        report[10] = 3; report[11] = 0x30;
    } else if (pro->step == SWITCH_PRO_FACTORY || pro->step == SWITCH_PRO_USER) {
        report[10] = 0x10;
        report[11] = pro->step == SWITCH_PRO_FACTORY ? 0x3d : 0x10;
        report[12] = pro->step == SWITCH_PRO_FACTORY ? 0x60 : 0x80;
        report[15] = pro->step == SWITCH_PRO_FACTORY ? 18 : 22;
    } else {
        report[10] = 0x30; report[11] = 1; /* Player 1 steady. */
    }
    ++pro->attempts;
    pro->waiting = 1;
    pro->retry_after = now + 100000;
    return 1;
}

int switch_pro_receive(SwitchPro *pro, const uint8_t *report, size_t size, DsState *state)
{
    DsState next;
    unsigned i, buttons;
    static const uint8_t right_bits[8] = {15, 12, 14, 13, 0, 0, 11, 9};
    static const uint8_t shared_bits[5] = {0, 3, 2, 1, 16};
    static const uint8_t left_bits[8] = {6, 4, 5, 7, 0, 0, 10, 8};
    if (!pro || !report || !state || size < 1) return -1;
    if (report[0] == 0x21) {
        if (size != 49 && size != 50) return -1;
        if (!pro->waiting || !(report[13] & 0x80)) return 0;
        if (pro->step == SWITCH_PRO_MODE && report[14] == 3) advance(pro);
        else if ((pro->step == SWITCH_PRO_FACTORY || pro->step == SWITCH_PRO_USER) &&
                 report[13] == 0x90 && report[14] == 0x10) {
            unsigned factory = pro->step == SWITCH_PRO_FACTORY;
            if (report[15] != (factory ? 0x3d : 0x10) ||
                report[16] != (factory ? 0x60 : 0x80) || report[17] || report[18] ||
                report[19] != (factory ? 18 : 22)) return 0;
            if (factory) {
                calibration(pro, report + 20, 0);
                calibration(pro, report + 29, 1);
            } else {
                if (report[20] == 0xb2 && report[21] == 0xa1) calibration(pro, report + 22, 0);
                if (report[31] == 0xb2 && report[32] == 0xa1) calibration(pro, report + 33, 1);
            }
            advance(pro);
        } else if (pro->step == SWITCH_PRO_LIGHTS && report[14] == 0x30) advance(pro);
        return 0;
    }
    if (size != SWITCH_PRO_REPORT_SIZE || report[0] != 0x30) return -1;
    ds_neutral(&next);
    buttons = 0;
    for (i = 0; i < 8; ++i) {
        if ((report[3] & (1u << i)) && i != 4 && i != 5) buttons |= 1u << right_bits[i];
        if ((report[5] & (1u << i)) && i != 4 && i != 5) buttons |= 1u << left_bits[i];
    }
    for (i = 0; i < 5; ++i)
        if (report[4] & (1u << i)) buttons |= 1u << shared_bits[i];
    next.buttons = buttons;
    next.l2 = buttons & (1u << 8) ? 255 : 0;
    next.r2 = buttons & (1u << 9) ? 255 : 0;
    next.lx = map_axis(&pro->axis[0], packed_axis(report + 6, 0), 0);
    next.ly = map_axis(&pro->axis[1], packed_axis(report + 6, 1), 1);
    next.rx = map_axis(&pro->axis[2], packed_axis(report + 9, 0), 0);
    next.ry = map_axis(&pro->axis[3], packed_axis(report + 9, 1), 1);
    pro->streaming = 1;
    if (pro->step == SWITCH_PRO_MODE) advance(pro); /* Full input also confirms mode. */
    *state = next;
    return 1;
}

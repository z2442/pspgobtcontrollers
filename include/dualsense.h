#ifndef PSPGOBT_DUALSENSE_H
#define PSPGOBT_DUALSENSE_H

#include <stddef.h>
#include <stdint.h>

#define DS_BT_REPORT_SIZE 78
#define DS3_REPORT_SIZE 49

/* Normalized buttons use the DS3 report's three digital button bytes. */
typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
} DsState;

void ds_neutral(DsState *state);
/* Report ID included, HIDP 0xA1 header excluded. Full Bluetooth mode only.
 * Failure leaves *state unchanged. Checks size, CRC and hat value. */
int ds_parse_bt(const uint8_t *report, size_t size, DsState *state);
/* Exact 10-byte basic Bluetooth report 0x01. No application CRC is present;
 * caller must establish the connected source's identity separately. */
int ds_parse_bt_simple(const uint8_t *report, size_t size, DsState *state);
/* Report ID included. Produces button/stick/pressure data; motion is neutral.
 * This is a wire report, NOT the undocumented PSP input driver's ABI. */
void ds_to_ds3(const DsState *state, uint8_t report[DS3_REPORT_SIZE]);
/* Full-mode output request, report ID included, HIDP 0xA2 excluded.
 * The transport must send this on the HID interrupt channel. */
void ds_build_enable(uint8_t report[DS_BT_REPORT_SIZE], uint8_t sequence);
uint32_t ds_crc32(uint8_t prefix, const uint8_t *data, size_t size);

#endif

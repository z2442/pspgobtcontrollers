#ifndef PSPGOBT_INPUT_STATE_H
#define PSPGOBT_INPUT_STATE_H
#include "dualsense.h"

#define GOBT_INPUT_TIMEOUT_US 250000u

/* 6.6x external-port payload described by crozone's controller demo/uOFW.
 * See THIRD_PARTY_NOTICES.md for Ryan Crosby's and uOFW's MIT notices.
 * Distinct name avoids conflicting with PSPSDK's older SceCtrlData type. */
typedef struct {
    uint32_t timestamp, buttons;
    uint8_t lx, ly, rx, ry, reserved[4];
    int32_t dpad_a, dpad_b, face_a, face_b, shoulder, trigger, tilt_a, tilt_b;
} GoBtCtrlData2;

typedef struct {
    DsState state;
    uint64_t received_us;
    int valid;
} GoBtInput;

/* The PSP adapter serializes all accesses to GoBtInput. No allocations or I/O. */
void gobt_input_reset(GoBtInput *input);
void gobt_input_update(GoBtInput *input, const DsState *state, uint64_t now);
/* Preserves the controller driver's timestamp. Returns 1 for fresh data,
 * 0 for neutral/disconnected. Expired data is invalidated permanently. */
int gobt_input_sample(GoBtInput *input, uint64_t now, GoBtCtrlData2 *frame);

#endif

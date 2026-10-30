#include "input_state.h"
#include <string.h>

typedef char frame_must_be_48_bytes[sizeof(GoBtCtrlData2) == 48 ? 1 : -1];

void gobt_input_reset(GoBtInput *input)
{
    memset(input, 0, sizeof(*input));
    ds_neutral(&input->state);
}

void gobt_input_update(GoBtInput *input, const DsState *state, uint64_t now)
{
    input->state = *state;
    input->received_us = now;
    input->valid = 1;
}

static int32_t pressure_pair(uint8_t low, uint8_t high)
{
    return (int32_t)((uint32_t)low | ((uint32_t)high << 16));
}

static uint8_t pressed(const DsState *state, unsigned bit)
{
    return (state->buttons & (1u << bit)) ? 255 : 0;
}

int gobt_input_sample(GoBtInput *input, uint64_t now, GoBtCtrlData2 *frame)
{
    DsState neutral;
    const DsState *state;
    uint32_t timestamp = frame->timestamp;
    if (input->valid && (now < input->received_us ||
                        now - input->received_us >= GOBT_INPUT_TIMEOUT_US))
        gobt_input_reset(input);
    ds_neutral(&neutral);
    state = input->valid ? &input->state : &neutral;
    memset(frame, 0, sizeof(*frame));
    frame->timestamp = timestamp;
    frame->buttons = state->buttons & 0x1ffffu;
    frame->lx = state->lx; frame->ly = state->ly;
    frame->rx = state->rx; frame->ry = state->ry;
    frame->reserved[0] = frame->reserved[1] = 128;
    frame->dpad_a = pressure_pair(pressed(state, 5), pressed(state, 7));
    frame->dpad_b = pressure_pair(pressed(state, 4), pressed(state, 6));
    frame->face_a = pressure_pair(pressed(state, 12), pressed(state, 13));
    frame->face_b = pressure_pair(pressed(state, 14), pressed(state, 15));
    frame->shoulder = pressure_pair(pressed(state, 10), pressed(state, 11));
    frame->trigger = pressure_pair(state->l2, state->r2);
    return input->valid;
}

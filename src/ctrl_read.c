#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman_kernel.h>
#include <psputilsforkernel.h>
#include <string.h>
#include "ctrl_read.h"
#include "input_state.h"
#include "native_sink.h"

typedef struct { uint32_t make, release, press, released; } CtrlLatch;
typedef int (*ReadBuffer)(void *, int, unsigned, unsigned);
typedef int (*ReadLatch)(CtrlLatch *);
static uintptr_t read_base, driver_state;
static const void *read_handler, *read_source;
static ReadBuffer read_buffer_original;
static ReadLatch peek_latch_original, read_latch_original;
static uint32_t read_stubs[3][4] __attribute__((aligned(16)));
static const unsigned read_offsets[] = {0x1e70, 0x1490, 0x153c};
static uint32_t remote_buttons, remote_make[2], remote_break[2];
static uint32_t hook_jump(unsigned index);

static uint32_t psp_buttons(uint32_t buttons)
{
    return (buttons & 0xf0ffu) | ((buttons & 0x500u) ? 0x100u : 0) |
           ((buttons & 0xa00u) ? 0x200u : 0);
}

void gobt_ctrl_read_update(const DsState *state)
{
    uint32_t buttons = psp_buttons(state->buttons);
    unsigned i;
    for (i = 0; i < 2; ++i) {
        remote_make[i] |= buttons & ~remote_buttons;
        remote_break[i] |= remote_buttons & ~buttons;
    }
    remote_buttons = buttons;
}

void gobt_ctrl_read_release(void)
{
    DsState neutral;
    ds_neutral(&neutral);
    gobt_ctrl_read_update(&neutral);
}

int gobt_ctrl_read_active(void)
{
    unsigned i;
    if (!read_base || *(const uint32_t *)(driver_state + 0x304) != (uintptr_t)read_handler ||
        *(const uint32_t *)(driver_state + 0x30c) != (uintptr_t)read_source) return 0;
    for (i = 0; i < 3; ++i) {
        const uint32_t *site = (const uint32_t *)(read_base + read_offsets[i]);
        if (site[0] != hook_jump(i) || site[1]) return 0;
    }
    return 1;
}

static unsigned user_call(void)
{
    /* Sony tests the sign of K1 << 11 when selecting its user buffers. */
    return (pspSdkGetK1() & 0x100000u) != 0;
}

static uint32_t allowed_buttons(unsigned user)
{
    uint32_t mask = *(const uint32_t *)(driver_state + 0x29c);
    if (user) mask &= *(const uint32_t *)(driver_state + 0x298);
    return mask;
}

static uint8_t merge_axis(uint8_t local, uint8_t remote)
{
    int a = (int)local - 128, b = (int)remote - 128;
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    return b > a ? remote : local;
}

static int read_buffer_latest(void *destination, int count, unsigned port, unsigned flags)
{
    int result = read_buffer_original(destination, count, port, flags);
    GoBtCtrlData2 snapshot = {0};
    unsigned user, stride, i;
    uint32_t mask, buttons;
    int interrupts;
    if (result <= 0 || result > count || result > 64 || port > 1) return result;
    interrupts = sceKernelCpuSuspendIntr();
    if (!gobt_ctrl_read_active()) { sceKernelCpuResumeIntr(interrupts); return result; }
    gobt_sink_snapshot(&snapshot);
    user = user_call();
    mask = allowed_buttons(user);
    stride = flags & 4 ? 48 : 16;
    buttons = psp_buttons(snapshot.buttons) & mask;
    for (i = 0; i < (unsigned)result; ++i) {
        GoBtCtrlData2 *frame = (GoBtCtrlData2 *)((uint8_t *)destination + i * stride);
        if (port == 1) {
            uint32_t timestamp = frame->timestamp;
            uint32_t raw_buttons = snapshot.buttons & mask & (user ? ~0x10000u : ~0u);
            if (stride == 48) *frame = snapshot;
            else {
                frame->lx = snapshot.lx; frame->ly = snapshot.ly;
                frame->rx = snapshot.rx; frame->ry = snapshot.ry;
            }
            frame->timestamp = timestamp;
            frame->buttons = flags & 1 ? ~raw_buttons : raw_buttons;
        } else {
            if (flags & 1) frame->buttons &= ~buttons;
            else frame->buttons |= buttons;
            /* Respect the caller's native digital/analog sampling mode. */
            if (*(const uint8_t *)(driver_state + 0xc + !user)) {
                frame->lx = merge_axis(frame->lx, snapshot.lx);
                frame->ly = merge_axis(frame->ly, snapshot.ly);
            }
        }
    }
    sceKernelCpuResumeIntr(interrupts);
    return result;
}

static int latch_latest(CtrlLatch *latch, int clear)
{
    int result = clear ? read_latch_original(latch) : peek_latch_original(latch);
    GoBtCtrlData2 snapshot = {0};
    unsigned user;
    uint32_t mask;
    int interrupts;
    if (result < 0) return result;
    interrupts = sceKernelCpuSuspendIntr();
    if (gobt_ctrl_read_active()) {
        gobt_sink_snapshot(&snapshot);
        user = user_call();
        mask = allowed_buttons(user);
        latch->make |= remote_make[user] & mask;
        latch->release |= remote_break[user] & mask;
        latch->press |= remote_buttons & mask;
        latch->released &= ~(remote_buttons & mask);
        if (clear) remote_make[user] = remote_break[user] = 0;
    }
    sceKernelCpuResumeIntr(interrupts);
    return result;
}

static int peek_latch_latest(CtrlLatch *latch) { return latch_latest(latch, 0); }
static int read_latch_latest(CtrlLatch *latch) { return latch_latest(latch, 1); }

static uint32_t hook_jump(unsigned index)
{
    const uintptr_t hooks[] = {(uintptr_t)read_buffer_latest,
                              (uintptr_t)peek_latch_latest, (uintptr_t)read_latch_latest};
    /* All entry points are kernel code; preserve the KSEG0 execution alias. */
    return 0x08000000u | (((hooks[index] | 0x80000000u) >> 2) & 0x03ffffffu);
}

static int registration_base(uintptr_t registration, uintptr_t *base)
{
    const uint32_t *code = (const uint32_t *)registration;
    uintptr_t state;
    if (registration < 0x880011f0u || registration > 0x8bffdf00u || (registration & 3)) return 0;
    if (code[0] != 0x27bdffd0u || code[1] != 0x2483ffffu || code[2] != 0xafb40020u ||
        (code[3] & 0xffff0000u) != 0x3c140000u || (code[4] & 0xffff0000u) != 0x26880000u ||
        code[0x74 / 4] != 0xacd20308u || code[0x7c / 4] != 0xacd30300u) return 0;
    *base = registration - 0x11f0;
    state = ((code[3] & 0xffffu) << 16) + (int16_t)code[4];
    return state == *base + 0x2890;
}

int gobt_ctrl_read_install(uintptr_t registration, const void *handler, const void *source)
{
    static const uint32_t buffer_signature[] = {0x27bdffd0, 0x24a3ffff, 0xafb10004, 0x3c088000};
    static const uint32_t latch_signature[] = {0x27bdfff0, 0xafb10004, 0x03608821, 0x001bdac0};
    uintptr_t base;
    unsigned i;
    int owned = 1, interrupts;
    if (!registration_base(registration, &base)) return 0;
    interrupts = sceKernelCpuSuspendIntr();
    for (i = 0; i < 3; ++i) {
        const uint32_t *site = (const uint32_t *)(base + read_offsets[i]);
        if (site[0] != hook_jump(i) || site[1]) owned = 0;
    }
    if (read_base == base && owned) { sceKernelCpuResumeIntr(interrupts); return 1; }
    for (i = 0; i < 3; ++i)
        if (memcmp((const void *)(base + read_offsets[i]),
                   i ? latch_signature : buffer_signature, sizeof(buffer_signature))) {
            sceKernelCpuResumeIntr(interrupts);
            return 0;
        }
    for (i = 0; i < 3; ++i) {
        const uint32_t *site = (const uint32_t *)(base + read_offsets[i]);
        read_stubs[i][0] = site[0];
        read_stubs[i][1] = site[1];
        read_stubs[i][2] = 0x08000000u | (((base + read_offsets[i] + 8) >> 2) & 0x03ffffffu);
        read_stubs[i][3] = 0;
    }
    read_buffer_original = (ReadBuffer)read_stubs[0];
    peek_latch_original = (ReadLatch)read_stubs[1];
    read_latch_original = (ReadLatch)read_stubs[2];
    driver_state = base + 0x2890;
    read_handler = handler; read_source = source;
    for (i = 0; i < 3; ++i) {
        uint32_t *site = (uint32_t *)(base + read_offsets[i]);
        site[0] = hook_jump(i); site[1] = 0;
    }
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    read_base = base;
    sceKernelCpuResumeIntr(interrupts);
    return 1;
}

void gobt_ctrl_read_shutdown(uintptr_t registration)
{
    uintptr_t base;
    unsigned i;
    int interrupts;
    if (!read_base || !registration_base(registration, &base) || base != read_base) return;
    interrupts = sceKernelCpuSuspendIntr();
    for (i = 0; i < 3; ++i) {
        uint32_t *site = (uint32_t *)(base + read_offsets[i]);
        if (site[0] == hook_jump(i) && site[1] == 0) {
            site[0] = read_stubs[i][0]; site[1] = read_stubs[i][1];
        }
    }
    read_base = 0;
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    sceKernelCpuResumeIntr(interrupts);
}

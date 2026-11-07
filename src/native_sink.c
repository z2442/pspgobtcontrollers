/* Native input registration based on the interface demonstrated by
 * Ryan Crosby's PSP-EmulatedControllerTest (MIT, 2025). See docs/research.md.
 * The user confirmed the DualSense input path on a 6.60 ARK-5 Go in v0.13. */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspintrman_kernel.h>
#include <pspsysevent.h>
#include <systemctrl.h>
#include "input_state.h"
#include "native_sink.h"
#include "bt_input_bridge.h"

typedef struct {
    int size;
    int (*copy)(void *source, GoBtCtrlData2 *destination);
} NativeHandler;
typedef int (*RegisterInput)(uint8_t port, NativeHandler *handler, void *source);
typedef unsigned (*SetPassthrough)(int mask);

static GoBtInput input;
static int ready, suspended, test_enabled;
static int shutting_down;
static SceUID test_thread_id = -1;
static uint64_t next_restore;

void pspgobtDisconnect(void)
{
    int interrupts = sceKernelCpuSuspendIntr();
    gobt_input_reset(&input);
    next_restore = 0;
    sceKernelCpuResumeIntr(interrupts);
}

int pspgobtSubmitReport(const uint8_t *report, size_t size)
{
    DsState state;
    int result;
    if (!ready || suspended || shutting_down) return -1;
    result = ds_parse_bt(report, size, &state);
    if (result < 0) return result;
    return gobt_sink_submit_state(&state);
}

int gobt_sink_submit_state(const DsState *state)
{
    int interrupts, result = 0;
    if (!state || !ready || suspended || shutting_down) return -1;
    interrupts = sceKernelCpuSuspendIntr();
    /* Recheck after parsing in case a power event cleared the session. */
    if (!suspended)
        gobt_input_update(&input, state, sceKernelGetSystemTimeWide());
    else
        result = -1;
    sceKernelCpuResumeIntr(interrupts);
    return result;
}

static int copy_input(void *source, GoBtCtrlData2 *destination)
{
    int interrupts = sceKernelCpuSuspendIntr();
    int fresh;
    (void)source;
    gobt_input_bridge_sample();
    fresh = gobt_input_sample(&input, sceKernelGetSystemTimeWide(), destination);
    sceKernelCpuResumeIntr(interrupts);
    return fresh ? 0 : -1;
}

/* Static lifetime: the controller driver retains a pointer to this structure. */
static NativeHandler handler = {sizeof(NativeHandler), copy_input};

static int power_event(int event, char *name, void *parameter, int *result)
{
    (void)name; (void)parameter; (void)result;
    if (shutting_down) return 0;
    if (event == 0x400 || event == 0x4000 || event == 0x400000) {
        int interrupts = sceKernelCpuSuspendIntr();
        suspended = event != 0x400000;
        gobt_input_reset(&input);
        next_restore = 0;
        sceKernelCpuResumeIntr(interrupts);
    }
    return 0;
}

static PspSysEventHandler power_handler = {
    .size = sizeof(PspSysEventHandler),
    .name = "PSPGoBTInput",
    .type_mask = 0x00ffff00,
    .handler = power_event
};

static int local_test(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* Read local input from a normal thread, never from the sampling callback.
     * Synthetic state never passes itself off as a received Bluetooth report. */
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    while (test_enabled) {
        SceCtrlData local;
        if (!suspended && sceCtrlPeekBufferPositive(&local, 1) == 1) {
            DsState state;
            int interrupts;
            ds_neutral(&state);
            if (local.Lx >= 188) state.buttons |= 1u << 5;
            if (local.Lx <= 68) state.buttons |= 1u << 7;
            if (local.Ly >= 188) state.buttons |= 1u << 6;
            if (local.Ly <= 68) state.buttons |= 1u << 4;
            interrupts = sceKernelCpuSuspendIntr();
            if (!suspended)
                gobt_input_update(&input, &state, sceKernelGetSystemTimeWide());
            sceKernelCpuResumeIntr(interrupts);
        }
        sceKernelDelayThread(16000);
    }
    return 0;
}

void gobt_sink_maintain(void)
{
    uint64_t now = sceKernelGetSystemTimeWide();
    int interrupts = sceKernelCpuSuspendIntr();
    int restore = ready && !suspended && input.valid &&
                  now >= input.received_us &&
                  now - input.received_us < GOBT_INPUT_TIMEOUT_US &&
                  now >= next_restore;
    if (restore) next_restore = now + 100000;
    sceKernelCpuResumeIntr(interrupts);
    /* popsman and other startup code may reset the driver after our initial
     * registration. Resolve again so driver replacement also gets a fresh
     * handler and mask; keep this work outside the sampling callback. */
    if (restore) gobt_sink_start(0);
}

int gobt_sink_start(int test_mode)
{
    RegisterInput register_input;
    SetPassthrough passthrough;
    int result;
    if (shutting_down) return -1;
    /* These are the actual 6.60/6.61 NIDs used by crozone's example.
     * Dynamic resolution avoids new import stubs and older SDK type conflicts. */
    register_input = (RegisterInput)sctrlHENFindFunction(
        "sceController_Service", "sceCtrl_driver", 0xe467bec8);
    passthrough = (SetPassthrough)sctrlHENFindFunction(
        "sceController_Service", "sceCtrl_driver", 0x6c86af22);
    if (!register_input || !passthrough) return -2;
    if (ready) {
        /* Bluetooth reconnect and controller-driver power handling can
         * replace the port handler or clear passthrough. Reclaim both for
         * each new accepted input session without registering power twice. */
        result = register_input(1, &handler, &input);
        if (result < 0) return result;
        passthrough(1);
        return 1;
    }
    gobt_input_reset(&input);
    result = sceKernelRegisterSysEventHandler(&power_handler);
    if (result < 0) return result;
    /* Create the test thread before claiming the port; start it only after
     * successful registration. A failed test start leaves a neutral sink. */
    if (test_mode) {
        test_thread_id = sceKernelCreateThread("PSPGoBTLocalTest", local_test,
                                               0x20, 0x1000, 0, NULL);
        if (test_thread_id < 0) {
            sceKernelUnregisterSysEventHandler(&power_handler);
            return test_thread_id;
        }
    }
    result = register_input(1, &handler, &input);
    if (result < 0) {
        if (test_thread_id >= 0) sceKernelDeleteThread(test_thread_id);
        test_thread_id = -1;
        sceKernelUnregisterSysEventHandler(&power_handler);
        return result;
    }
    passthrough(1);
    ready = 1;
    next_restore = sceKernelGetSystemTimeWide() + 100000;
    if (test_mode) {
        test_enabled = 1;
        result = sceKernelStartThread(test_thread_id, 0, NULL);
        if (result < 0) {
            test_enabled = 0;
            sceKernelDeleteThread(test_thread_id);
            test_thread_id = -1;
            return result;
        }
    }
    return 0;
}

void gobt_sink_shutdown(void)
{
    RegisterInput register_input;
    SetPassthrough passthrough;
    const u32 *registration, *setter;
    uintptr_t state, mask;
    int was_ready, interrupts = sceKernelCpuSuspendIntr();
    was_ready = ready;
    shutting_down = 1;
    ready = test_enabled = 0;
    gobt_input_reset(&input);
    sceKernelCpuResumeIntr(interrupts);
    if (!was_ready) return;
    register_input = (RegisterInput)sctrlHENFindFunction(
        "sceController_Service", "sceCtrl_driver", 0xe467bec8);
    passthrough = (SetPassthrough)sctrlHENFindFunction(
        "sceController_Service", "sceCtrl_driver", 0x6c86af22);
    registration = (const u32 *)register_input;
    setter = (const u32 *)passthrough;
    /* Decode the captured 6.6x accessors before checking retained pointers.
     * Never detach a handler that another plugin has installed in our place. */
    if ((uintptr_t)registration >= 0x88000000u && (uintptr_t)registration <= 0x8bffff80u &&
        (uintptr_t)setter >= 0x88000000u && (uintptr_t)setter <= 0x8bfffff0u &&
        !(((uintptr_t)registration | (uintptr_t)setter) & 3) &&
        registration[0] == 0x27bdffd0u && registration[1] == 0x2483ffffu &&
        registration[2] == 0xafb40020u && (registration[3] & 0xffff0000u) == 0x3c140000u &&
        (registration[4] & 0xffff0000u) == 0x26880000u &&
        registration[0x74 / 4] == 0xacd20308u && registration[0x7c / 4] == 0xacd30300u &&
        (setter[0] & 0xffff0000u) == 0x3c030000u && setter[1] == 0x00001021u &&
        setter[2] == 0x03e00008u && (setter[3] & 0xffff0000u) == 0xac640000u) {
        state = ((registration[3] & 0xffffu) << 16) + (int16_t)registration[4];
        mask = ((setter[0] & 0xffffu) << 16) + (int16_t)setter[3];
        if (state >= 0x88000000u && state <= 0x8bfffcecu && !(state & 3) && mask == state + 0x300) {
            interrupts = sceKernelCpuSuspendIntr();
            if (*(const u32 *)(state + 0x304) == (u32)&handler &&
                *(const u32 *)(state + 0x30c) == (u32)&input) {
                /* An existing port takes the verified nonallocating branch. */
                register_input(1, NULL, NULL);
                passthrough(*(const u32 *)mask & ~1u);
            }
            sceKernelCpuResumeIntr(interrupts);
        }
    }
    sceKernelUnregisterSysEventHandler(&power_handler);
}

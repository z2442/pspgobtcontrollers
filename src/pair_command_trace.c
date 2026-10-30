#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman_kernel.h>
#include <psputilsforkernel.h>
#include <stdint.h>
#include <string.h>
#include "pairing_route.h"
#include "pair_command_trace.h"
#include "probe_log.h"

static int (*original_command)(const void *, void *, int);
static unsigned emitted;

/* Called only at the two guarded kernel call sites, with Sony's own stack
 * descriptors. Never inspect their payload pointers: they may contain PINs,
 * peer addresses, or pairing material. No user-mode export is provided. */
static int trace_pair_command(const void *request, void *response, int mode)
{
    const u16 *req = request;
    const u16 *reply = response;
    unsigned event, command, bytes, expected, k1;
    int result, interrupts;
    interrupts = sceKernelCpuSuspendIntr();
    event = emitted;
    if (event < 64) ++emitted;
    sceKernelCpuResumeIntr(interrupts);
    if (event >= 64) return original_command(request, response, mode);
    command = req[0];
    bytes = req[1];
    expected = reply[0];
    k1 = pspSdkSetK1(0);
    gobt_log("bt-pair-command begin event=%u command=%04X bytes=%u expected=%04X mode=%d\n",
             event + 1, command, bytes, expected, mode);
    pspSdkSetK1(k1);
    result = original_command(request, response, mode);
    k1 = pspSdkSetK1(0);
    if (result >= 0)
        gobt_log("bt-pair-command end event=%u command=%04X result=%08X status=%04X\n",
                 event + 1, command, (unsigned)result, reply[2]);
    else
        gobt_log("bt-pair-command end event=%u command=%04X result=%08X status=unavailable\n",
                 event + 1, command, (unsigned)result);
    pspSdkSetK1(k1);
    return result;
}

int gobt_install_pair_command_trace(SceModule *module, unsigned long firmware)
{
    const unsigned offsets[] = {GOBT_PAIR_PARAMETER_CALL_OFFSET, GOBT_PAIR_START_CALL_OFFSET};
    unsigned i;
    u32 target = (u32)trace_pair_command;
    int interrupts;
    if (!module || strcmp(module->modname, "sceBSMan")) return -1;
    if (!gobt_pair_commands_supported((uint32_t)firmware, (const uint8_t *)module->text_addr,
                                      module->text_size, (uint32_t)module->text_addr)) return -2;
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        if (((module->text_addr + offsets[i] + 4) ^ target) & 0xf0000000u) return -3;
    /* The common function's instructions stay intact. Replace only these
     * two jal targets and preserve their original delay slots. */
    original_command = (void *)(module->text_addr + GOBT_PAIR_COMMAND_OFFSET);
    interrupts = sceKernelCpuSuspendIntr();
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        _sw(0x0c000000u | ((target >> 2) & 0x03ffffffu), module->text_addr + offsets[i]);
    sceKernelCpuResumeIntr(interrupts);
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    gobt_log("bt-pair-command trace installed parameter-site=%08lX start-site=%08lX limit=64\n",
             module->text_addr + offsets[0], module->text_addr + offsets[1]);
    return 0;
}

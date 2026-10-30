#include <pspkernel.h>
#include <pspsdk.h>
#include <pspkerror.h>
#include <pspintrman_kernel.h>
#include <psputilsforkernel.h>
#include <systemctrl.h>
#include <stdint.h>
#include <string.h>
#include "bsman_trace.h"
#include "pairing_route.h"
#include "probe_log.h"
#include "xmb_pair_route.h"

static u32 replacement;

static int user_object(const void *object, const void *address)
{
    uintptr_t p = (uintptr_t)object;
    return !(p & 3) && p >= 0x08800000u && p <= 0x0c000000u - 88 &&
           (uintptr_t)address == p + 79;
}

int pspgobtPairFallback(const void *object, const void *address)
{
    unsigned k1;
    int selected = 0, result;
    SceUID block;
    void *parameter;
    const uint8_t *record;
    /* Inspect the preceding scan record only in the guarded object-method
     * context. An arbitrary six-byte address does not imply an 84-byte record. */
    if (!user_object(object, address)) return pspgobtTraceRegisterAddress(address);
    record = (const uint8_t *)object + 4;
    k1 = pspSdkSetK1(0);
    selected = gobt_pair_scan_record(record, GOBT_SCAN_RECORD_BYTES);
    if (!selected) {
        gobt_log("bt-pair-route unchanged record84=%u name-match=0; using original address registration\n",
                 *(const uint32_t *)record == GOBT_SCAN_RECORD_BYTES);
        pspSdkSetK1(k1);
        return pspgobtTraceRegisterAddress(address);
    }
    /* The native helper copies 32 parameter bytes and appends a UTF-16
     * terminator to its 34-byte command payload. Keep a terminated buffer
     * in user RAM so Sony's original K1 pointer checks remain applicable. */
    block = sceKernelAllocPartitionMemory(2, "PSPGoBTPairParam", PSP_SMEM_Low,
                                          GOBT_PAIR_PARAMETER_BYTES, NULL);
    if (block < 0) {
        gobt_log("bt-pair-route parameter allocation failed result=%08X\n", block);
        pspSdkSetK1(k1);
        return block;
    }
    parameter = sceKernelGetBlockHeadAddr(block);
    if (!parameter) {
        sceKernelFreePartitionMemory(block);
        pspSdkSetK1(k1);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }
    gobt_pair_default_parameter(parameter, GOBT_PAIR_PARAMETER_BYTES);
    gobt_log("bt-pair-route target=DualSense record84=1 parameter=xmb-default class=%02X%02X%02X; using native full-record registration\n",
             record[74], record[73], record[72]);
    pspSdkSetK1(k1);
    result = pspgobtTraceRegisterScan(record, parameter);
    k1 = pspSdkSetK1(0);
    sceKernelFreePartitionMemory(block);
    gobt_log("bt-pair-route completed result=%08X; native result preserved\n", (unsigned)result);
    pspSdkSetK1(k1);
    /* In particular, do not fall back to address-only registration on failure:
     * that could recreate a saved entry without completing this procedure. */
    return result;
}

int gobt_install_pair_route(SceModule *module, unsigned long firmware)
{
    u32 site;
    int interrupts;
    if (!module || strcmp(module->modname, "bluetooth_plugin_module")) return -1;
    if (!gobt_pair_route_supported((uint32_t)firmware, (const uint8_t *)module->text_addr,
                                   module->text_size, (uint32_t)module->text_addr)) return -2;
    if (!gobt_registration_trace_ready()) return -3;
    if (!replacement) replacement = sctrlHENMakeSyscallStub(pspgobtPairFallback);
    if (!replacement) return -4;
    site = module->text_addr + GOBT_PAIR_CALL_OFFSET;
    if (((site + 4) ^ replacement) & 0xf0000000u) return -5;
    interrupts = sceKernelCpuSuspendIntr();
    _sw(0x0c000000u | ((replacement >> 2) & 0x03ffffffu), site);
    sceKernelCpuResumeIntr(interrupts);
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    gobt_log("bt-pair-route installed site=%08lX stub=%08lX; guarded DualSense full-record registration enabled\n",
             site, replacement);
    return 0;
}

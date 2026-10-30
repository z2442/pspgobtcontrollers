#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman_kernel.h>
#include <psputilsforkernel.h>
#include <systemctrl.h>
#include <stdint.h>
#include <string.h>
#include "hid_route.h"
#include "probe_log.h"
#include "xmb_hid_route.h"
#include "bt_input_bridge.h"
#include "session_trace.h"

static u32 decision_stub;
static u32 stored_stub;
static unsigned routed;
static unsigned skipped;

static int user_range(const void *buffer, size_t size)
{
    uintptr_t p = (uintptr_t)buffer;
    return !(p & 3) && p >= 0x08800000u && p <= 0x0c000000u - size;
}

int pspgobtRouteHid(void *profile, const void *info)
{
    int changed;
    unsigned k1, hit;
    int interrupts;
    if (!user_range(profile, GOBT_PROFILE_BYTES) ||
        !user_range(info, GOBT_DEVICE_INFO_BYTES)) return 0;
    k1 = pspSdkSetK1(0);
    unsigned previous_type = ((const u32 *)profile)[1];
    changed = gobt_route_hid_record(profile, GOBT_PROFILE_BYTES, info, GOBT_DEVICE_INFO_BYTES);
    gobt_session_trace(GOBT_TRACE_MODE, ((const u32 *)profile)[0], previous_type, changed, ((const u16 *)info)[36]);
    if (changed) {
        interrupts = sceKernelCpuSuspendIntr();
        hit = routed++;
        sceKernelCpuResumeIntr(interrupts);
        if (hit < 16)
            gobt_log("xmb-hid-route hit=%u type=9 field48=0402; HID controller mode selected\n", hit + 1);
    } else {
        interrupts = sceKernelCpuSuspendIntr();
        hit = skipped++;
        sceKernelCpuResumeIntr(interrupts);
        if (hit < 16)
            gobt_log("xmb-hid-route skipped=%u type=%u record80=%u field48=%04X; record unchanged\n",
                     hit + 1, (unsigned)((const uint32_t *)profile)[1],
                     *(const uint32_t *)info == GOBT_DEVICE_INFO_BYTES,
                     ((const uint16_t *)info)[36]);
    }
    pspSdkSetK1(k1);
    return changed;
}

int pspgobtRouteStoredHid(void *profile, const void *info, unsigned device)
{
    unsigned k1;
    int selected;
    if (!user_range(profile, GOBT_PROFILE_BYTES) || device < 1 || device > 8 ||
        (info && !user_range(info, GOBT_DEVICE_INFO_BYTES))) return 0;
    k1 = pspSdkSetK1(0);
    unsigned previous_type = ((const u32 *)profile)[1];
    if (info) selected = gobt_route_hid_record(profile, GOBT_PROFILE_BYTES, info, GOBT_DEVICE_INFO_BYTES);
    else selected = gobt_input_bridge_known(device) && gobt_route_hid_cached(profile, GOBT_PROFILE_BYTES);
    gobt_session_trace(GOBT_TRACE_MODE, device, previous_type, selected, info ? ((const u16 *)info)[36] : 0);
    gobt_log("xmb-hid-mode saved slot=%u metadata=%u selected=%u\n", device, info != NULL, selected);
    pspSdkSetK1(k1);
    return selected;
}

int gobt_install_hid_route(SceModule *module, unsigned long firmware)
{
    SceUID block;
    u32 *code, *stored, *label, *store, call_site, base;
    int result, interrupts;
    if (!module || strcmp(module->modname, "bluetooth_plugin_module")) return -1;
    if (!gobt_hid_route_supported((uint32_t)firmware, (const uint8_t *)module->text_addr,
                                  module->text_size, (uint32_t)module->text_addr)) return -2;
    if (!gobt_hid_mode_supported((const uint8_t *)module->text_addr, module->text_size,
                                 module->text_addr)) return -2;
    if (!decision_stub) decision_stub = sctrlHENMakeSyscallStub(pspgobtRouteHid);
    if (!stored_stub) stored_stub = sctrlHENMakeSyscallStub(pspgobtRouteStoredHid);
    if (!decision_stub || !stored_stub) return -3;
    block = sceKernelAllocPartitionMemory(2, "PSPGoBTHidRoute", PSP_SMEM_Low,
                 (GOBT_ROUTE_CODE_WORDS + GOBT_STORED_CODE_WORDS + GOBT_LABEL_CODE_WORDS + GOBT_STORE_CODE_WORDS) * sizeof(u32), NULL);
    if (block < 0) return block;
    code = sceKernelGetBlockHeadAddr(block);
    if (!code) {
        sceKernelFreePartitionMemory(block);
        return -4;
    }
    base = module->text_addr;
    stored = code + GOBT_ROUTE_CODE_WORDS;
    label = stored + GOBT_STORED_CODE_WORDS;
    store = label + GOBT_LABEL_CODE_WORDS;
    call_site = base + GOBT_ROUTE_CALL_OFFSET;
    if (((call_site + 4) ^ (u32)code) & 0xf0000000u) {
        sceKernelFreePartitionMemory(block);
        return -5;
    }
    result = gobt_build_route_code((uint32_t *)code, (u32)code, decision_stub,
                                    base + GOBT_ROUTE_NATIVE_OFFSET, base + GOBT_ROUTE_FACTORY_OFFSET);
    if (result >= 0) result = gobt_build_stored_hid_code(stored, (u32)stored, stored_stub, base);
    if (result >= 0) result = gobt_build_hid_label_code(label, (u32)label, base);
    if (result >= 0) result = gobt_build_hid_store_code(store, (u32)store, base);
    if (result < 0) {
        sceKernelFreePartitionMemory(block);
        return -6;
    }
    /* Keep the user stub alive until reboot, like the resident module and its
     * chained loader callback. Only user code calls user profile constructors. */
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    interrupts = sceKernelCpuSuspendIntr();
    _sw(0x0c000000u | (((u32)code >> 2) & 0x03ffffffu), call_site);
    _sw(0x0c000000u | (((u32)stored >> 2) & 0x03ffffffu), base + GOBT_STORED_CALL_OFFSET);
    _sw(0x08000000u | (((u32)label >> 2) & 0x03ffffffu), base + GOBT_LABEL_ENTRY_OFFSET);
    _sw(0, base + GOBT_LABEL_ENTRY_OFFSET + 4);
    _sw(0x08000000u | (((u32)store >> 2) & 0x03ffffffu), base + GOBT_STORE_ENTRY_OFFSET);
    _sw(0, base + GOBT_STORE_ENTRY_OFFSET + 4);
    sceKernelCpuResumeIntr(interrupts);
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    gobt_log("xmb-hid-route installed site=%08lX trampoline=%08lX; DualSense name/type/capability guards active\n",
             call_site, (u32)code);
    return 0;
}

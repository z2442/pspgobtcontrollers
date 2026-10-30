#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman_kernel.h>
#include <psputilsforkernel.h>
#include <systemctrl.h>
#include <stdint.h>
#include <string.h>
#include "bsman_trace.h"
#include "probe_log.h"
#include "probe_policy.h"
#include "bt_input_bridge.h"
#include "session_trace.h"

enum { INFO, REGISTER_SCAN, REGISTER_ADDRESS, CONNECT, IDENTITY, DISCONNECT, API_COUNT };
typedef struct {
    u32 nid;
    const char *name;
    void *wrapper;
    u32 original, replacement_stub;
    unsigned emitted;
} TraceApi;

static TraceApi apis[API_COUNT] = {
    {0xcbd232d1, "device-info", pspgobtTraceDeviceInfo, 0, 0, 0},
    {0x8ac2f7b8, "register-scan", pspgobtTraceRegisterScan, 0, 0, 0},
    {0x2d649923, "register-address", pspgobtTraceRegisterAddress, 0, 0, 0},
    {0xf188a405, "hid-connect", pspgobtTraceConnect, 0, 0, 0},
    {0x1238f36e, "identity", pspgobtTraceIdentity, 0, 0, 0},
    {0x68327a27, "hid-disconnect", pspgobtTraceDisconnect, 0, 0, 0},
};

int gobt_registration_trace_ready(void)
{
    unsigned i;
    const unsigned required[] = {REGISTER_SCAN, REGISTER_ADDRESS};
    SceModule *provider = sceKernelFindModuleByName("sceBSMan");
    if (!provider) return 0;
    for (i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        TraceApi *api = &apis[required[i]];
        if (!api->original || api->original < provider->text_addr ||
            api->original - provider->text_addr >= provider->text_size ||
            !gobt_bsman_api_supported((uint32_t)api->nid, (const uint8_t *)api->original,
                                      provider->text_size - (api->original - provider->text_addr)))
            return 0;
    }
    return 1;
}

static unsigned ticket(unsigned api)
{
    unsigned event;
    int interrupts = sceKernelCpuSuspendIntr();
    event = apis[api].emitted;
    if (event < 64) ++apis[api].emitted;
    sceKernelCpuResumeIntr(interrupts);
    return event < 64 ? event + 1 : 0;
}

static unsigned begin(unsigned api)
{
    unsigned event = ticket(api);
    if (event) {
        unsigned k1 = pspSdkSetK1(0);
        gobt_log("bt-api begin op=%s event=%u\n", apis[api].name, event);
        pspSdkSetK1(k1);
    }
    return event;
}

static void finish(unsigned api, unsigned event, int result)
{
    if (event) {
        unsigned k1 = pspSdkSetK1(0);
        gobt_log("bt-api end op=%s event=%u result=%08X\n",
                 apis[api].name, event, (unsigned)result);
        pspSdkSetK1(k1);
    }
}

static int user_buffer(const void *buffer, size_t size, uintptr_t alignment)
{
    uintptr_t pointer = (uintptr_t)buffer;
    return pointer >= 0x08800000u && pointer <= 0x0c000000u - size &&
           (pointer & (alignment - 1)) == 0;
}

int pspgobtTraceDeviceInfo(unsigned int device, void *record)
{
    int (*original)(unsigned int, void *) = (void *)apis[INFO].original;
    int result;
    unsigned event, k1, has_record = 0, field72 = 0;
    /* Preserve the caller's K1 for Sony's pointer/permission checks. */
    if (!original) return -1;
    result = original(device, record);
    event = ticket(INFO);
    if (!event) return result;
    k1 = pspSdkSetK1(0);
    if (result >= 0 && user_buffer(record, 80, 4) && *(const uint32_t *)record == 80) {
        has_record = 1;
        field72 = ((const uint16_t *)record)[36];
    }
    /* Offset 0x06 proved to be the first UTF-16 name character, so v0.3
     * stops logging it. No peer address or PIN is recorded. */
    gobt_log("device-info event=%u device=%08X result=%08X record80=%u field48=%04X\n",
             event, device, (unsigned)result, has_record, field72);
    pspSdkSetK1(k1);
    return result;
}

int pspgobtTraceRegisterScan(const void *record, const void *parameter)
{
    int (*original)(const void *, const void *) = (void *)apis[REGISTER_SCAN].original;
    unsigned event;
    int result;
    if (!original) return -1;
    gobt_session_trace(GOBT_TRACE_REGISTER, 0, 0, 0, 0);
    event = begin(REGISTER_SCAN);
    result = original(record, parameter);
    gobt_session_trace(GOBT_TRACE_REGISTER_DONE, 0, result, 0, 0);
    finish(REGISTER_SCAN, event, result);
    return result;
}

int pspgobtTraceRegisterAddress(const void *address)
{
    int (*original)(const void *) = (void *)apis[REGISTER_ADDRESS].original;
    unsigned event;
    int result;
    if (!original) return -1;
    gobt_session_trace(GOBT_TRACE_REGISTER, 0, 1, 0, 0);
    event = begin(REGISTER_ADDRESS);
    result = original(address);
    gobt_session_trace(GOBT_TRACE_REGISTER_DONE, 0, result, 1, 0);
    finish(REGISTER_ADDRESS, event, result);
    return result;
}

int pspgobtTraceConnect(unsigned int device, int mode)
{
    int (*original)(unsigned int, int) = (void *)apis[CONNECT].original;
    unsigned event;
    int result, selected;
    if (!original) return -1;
    gobt_session_trace(GOBT_TRACE_CONNECT, device, mode, 0, 0);
    if (gobt_input_bridge_connect_ready(device) < 0) {
        gobt_session_trace(GOBT_TRACE_CONNECT_DONE, device, -1, 0, 0);
        return -1;
    }
    selected = gobt_input_bridge_prepare(device);
    if (selected == 1) mode = 0; /* generic HID; retain native Bluetooth result */
    event = begin(CONNECT);
    if (event) {
        unsigned k1 = pspSdkSetK1(0);
        gobt_log("bt-connect event=%u device=%u mode=%d\n", event, device, mode);
        pspSdkSetK1(k1);
    }
    /* Incoming connections need no call here. This XMB request uses generic
     * HID for a classified DualSense; other devices keep the supplied mode. */
    result = original(device, mode);
    gobt_session_trace(GOBT_TRACE_CONNECT_DONE, device, result, mode, selected);
    finish(CONNECT, event, result);
    if (result >= 0) gobt_input_bridge_connected(device);
    return result;
}

int pspgobtTraceDisconnect(unsigned int device)
{
    int (*original)(unsigned) = (void *)apis[DISCONNECT].original;
    unsigned token;
    int result;
    if (!original) return -1;
    token = gobt_input_bridge_disconnecting(device);
    gobt_session_trace(GOBT_TRACE_DISCONNECT, device, token, 0, 0);
    result = original(device); /* Preserve Sony's result and caller's K1. */
    gobt_session_trace(GOBT_TRACE_DISCONNECT_DONE, device, result, token, 0);
    gobt_input_bridge_disconnect_result(device, token, result);
    return result;
}

int pspgobtTraceIdentity(unsigned int device, void *vendor, void *product, void *extra)
{
    int (*original)(unsigned int, void *, void *, void *) = (void *)apis[IDENTITY].original;
    unsigned event;
    int result;
    if (!original) return -1;
    event = begin(IDENTITY);
    result = original(device, vendor, product, extra);
    finish(IDENTITY, event, result);
    if (event && result >= 0 && user_buffer(vendor, 2, 2) && user_buffer(product, 2, 2)) {
        unsigned k1 = pspSdkSetK1(0);
        gobt_log("bt-identity event=%u vendor=%04X product=%04X\n", event,
                 (unsigned)*(const uint16_t *)vendor, (unsigned)*(const uint16_t *)product);
        pspSdkSetK1(k1);
    }
    return result;
}

static int install_import(SceModule *module, SceModule *provider, unsigned api)
{
    unsigned offset = 0;
    TraceApi *trace = &apis[api];
    trace->original = sctrlHENFindFunction("sceBSMan", "sceBSMan", trace->nid);
    if (!trace->original) return -3;
    if (trace->original < provider->text_addr ||
        trace->original - provider->text_addr >= provider->text_size ||
        !gobt_bsman_api_supported((uint32_t)trace->nid, (const uint8_t *)trace->original,
                                 provider->text_size - (trace->original - provider->text_addr)))
        return -9;
    while (offset + 20 <= module->stub_size) {
        SceLibraryStubTable *entry = (void *)((u8 *)module->stub_top + offset);
        unsigned bytes = entry->len * 4u, i;
        if (bytes < 20 || bytes > module->stub_size - offset) return -4;
        if (entry->libname && !strcmp(entry->libname, "sceBSMan") &&
            entry->nidtable && entry->stubtable) {
            for (i = 0; i < entry->stubcount; ++i) {
                u32 *stub;
                int interrupts;
                if (entry->nidtable[i] != trace->nid) continue;
                stub = (u32 *)entry->stubtable + 2 * i;
                if ((u32)stub < module->text_addr ||
                    (u32)stub - module->text_addr > module->text_size - 8) return -5;
                if (stub[0] != 0x03e00008 ||
                    (stub[1] & 0xfc00003fu) != 0x0000000cu || stub[1] == 0x0000054c)
                    return -6;
                if (!trace->replacement_stub)
                    trace->replacement_stub = sctrlHENMakeSyscallStub(trace->wrapper);
                if (!trace->replacement_stub) return -7;
                interrupts = sceKernelCpuSuspendIntr();
                stub[1] = ((const u32 *)trace->replacement_stub)[1];
                sceKernelCpuResumeIntr(interrupts);
                sceKernelDcacheWritebackInvalidateAll();
                sceKernelIcacheInvalidateAll();
                gobt_log("bt-api trace installed op=%s nid=%08lX import=%08lX original=%08lX limit=64\n",
                         trace->name, trace->nid, (u32)stub, trace->original);
                return 0;
            }
        }
        offset += bytes;
    }
    return -8;
}

int gobt_trace_device_info(SceModule *module, unsigned long firmware)
{
    unsigned api;
    int info_result = -3;
    SceModule *provider;
    if (!module || strcmp(module->modname, "bluetooth_plugin_module")) return -1;
    if (!gobt_device_info_trace_supported((uint32_t)firmware,
                                         (const uint8_t *)module->text_addr,
                                         module->text_size)) return -2;
    provider = sceKernelFindModuleByName("sceBSMan");
    if (!provider) return -3;
    for (api = 0; api < API_COUNT; ++api) {
        int result = install_import(module, provider, api);
        gobt_log("bt-api trace setup op=%s result=%d\n", apis[api].name, result);
        if (api == INFO) info_result = result;
    }
    return info_result;
}

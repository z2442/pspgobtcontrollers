#include <pspkernel.h>
#include <psputilsforkernel.h>
#include <systemctrl.h>
#include <string.h>
#include "mapping_store.h"
#include "mapping_patch.h"
#include "session_trace.h"

typedef struct {
    GoBtMappingUiArgs args;
    SceUID owner;
    uint32_t firmware;
} UiLoad;

/* One helper serves one Bluetooth module instance. The XMB loads that module
 * again for each visit to Settings, so remember the previous pair. */
static SceUID helper_uid = -1, helper_owner = -1;
static uint32_t helper_base;
static uint32_t gateway;

static int owner_live(const UiLoad *load)
{
    SceModule *owner = sceKernelFindModuleByUID(load->owner);
    return !gobt_mapping_closing() && owner && owner->text_addr == load->args.base &&
           !strcmp(owner->modname, "bluetooth_plugin_module") &&
           gobt_mapping_supported(load->firmware, (const uint8_t *)owner->text_addr,
                                   owner->text_size, owner->text_addr);
}

/* A helper may be unloaded only when the module holding its hooks is gone:
 * those hooks lived in that module's own image and disappeared with it. */
static void retire_helper(void)
{
    SceModule *owner;
    int status;
    if (helper_uid < 0) return;
    owner = sceKernelFindModuleByUID(helper_owner);
    if (owner && owner->text_addr == helper_base) return;
    if (sceKernelStopModule(helper_uid, 0, NULL, &status, NULL) >= 0)
        sceKernelUnloadModule(helper_uid);
    helper_uid = -1;
}

static int load_ui(SceSize size, void *data)
{
    UiLoad load;
    SceUID uid;
    int status;
    if (size != sizeof(load) || !data) goto done;
    memcpy(&load, data, sizeof(load));
    retire_helper();
    if (!owner_live(&load) || helper_uid >= 0) goto done;
    uid = sceKernelLoadModule("ef0:/seplugins/pspgobt/mapping_ui.prx", 0, NULL);
    if (uid < 0) {
        gobt_session_trace(GOBT_TRACE_MAPPING, 2, (unsigned)uid, 0, 0);
        goto done;
    }
    status = -1;
    if (!owner_live(&load) ||
        sceKernelStartModule(uid, sizeof(load.args), &load.args, &status, NULL) < 0 || status < 0) {
        gobt_session_trace(GOBT_TRACE_MAPPING, 2, (unsigned)uid, (unsigned)status, 0);
        sceKernelUnloadModule(uid);
        goto done;
    }
    gobt_session_trace(GOBT_TRACE_MAPPING, 2, (unsigned)uid, (unsigned)status, 1);
    helper_uid = uid;
    helper_owner = load.owner;
    helper_base = load.args.base;
done:
    sceKernelExitDeleteThread(0);
    return 0;
}

void gobt_mapping_install(SceModule *module, unsigned long firmware)
{
    UiLoad load;
    SceUID thread;
    /* The helper lives in user memory, where native PAF callbacks run.
     * Loadexec removes it together with Bluetooth's other XMB widgets. */
    if (gobt_mapping_closing() || !module || strcmp(module->modname, "bluetooth_plugin_module")) return;
    {
        int supported = gobt_mapping_supported(firmware, (const uint8_t *)module->text_addr,
                                               module->text_size, module->text_addr);
        gobt_session_trace(GOBT_TRACE_MAPPING, 1, supported, firmware, module->text_size);
        if (!supported || gobt_mapping_start() < 0) return;
    }
    /* ARK allocates a fresh user stub per call. Make one, and make sure the
     * CPU fetches its written words rather than stale cache lines. */
    if (!gateway) {
        gateway = sctrlHENMakeSyscallStub(pspgobtMappingService);
        if (!gateway) return;
        sceKernelDcacheWritebackInvalidateAll();
        sceKernelIcacheInvalidateAll();
    }
    load.args.abi = GOBT_MAPPING_UI_ABI;
    load.args.base = module->text_addr;
    load.args.size = module->text_size;
    load.args.gateway = gateway;
    load.owner = module->modid;
    load.firmware = firmware;
    /* Never recurse into ModuleMgr from its start-module callback. ThreadMan
     * copies these arguments to the worker, which may wait on ModuleMgr while
     * the callback returns and releases its loader locks. */
    thread = sceKernelCreateThread("PSPGoBTMappingUI", load_ui, 0x20, 0x2000, 0, NULL);
    if (thread < 0) return;
    if (sceKernelStartThread(thread, sizeof(load), &load) < 0) sceKernelDeleteThread(thread);
}

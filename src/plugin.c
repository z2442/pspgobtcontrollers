#include <pspkernel.h>
#include <psploadcore.h>
#include <pspsysmem_kernel.h>
#include <psputilsforkernel.h>
#include <pspintrman_kernel.h>
#include <pspsysclib.h>
#include <systemctrl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "xmb_patch.h"
#include "native_sink.h"
#include "probe_log.h"
#include "probe_policy.h"
#include "bsman_trace.h"
#include "xmb_hid_route.h"
#include "xmb_pair_route.h"
#include "pair_command_trace.h"
#include "bt_input_bridge.h"
#include "session_trace.h"

/* Resident: another plugin may chain our callback. Disable and reboot to
 * remove it; do not tear down a callback that may still be referenced. */
PSP_MODULE_INFO("PSPGoBT_Probe", 0x1001, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

#define ROOT "ef0:/seplugins/pspgobt"
static STMOD_HANDLER previous;
static u32 firmware;
static int enable_filter;
static SceUID seen[128];
static unsigned seen_count;
static int leaving;

#if PSPGOBT_DEBUG
typedef struct {
    char bytes[384];
    unsigned used;
} LogBuffer;

static void log_character(void *context, int ch)
{
    LogBuffer *buffer = context;
    /* prnt also sends 0x200/0x201 start/end notifications. */
    if (ch >= 0 && ch < 256 && buffer->used < sizeof(buffer->bytes))
        buffer->bytes[buffer->used++] = (char)ch;
}

void gobt_log(const char *format, ...)
{
    LogBuffer buffer = { .used = 0 };
    va_list args;
    SceUID fd;
    va_start(args, format);
    prnt(log_character, &buffer, format, args);
    va_end(args);
    fd = sceIoOpen(ROOT "/probe.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, buffer.bytes, buffer.used);
    sceIoClose(fd);
}

static void dump_exports(SceModule *mod)
{
    unsigned offset = 0;
    while (offset + sizeof(SceLibraryEntryTable) <= mod->ent_size) {
        SceLibraryEntryTable *entry = (void *)((u8 *)mod->ent_top + offset);
        unsigned bytes = entry->len * 4u;
        unsigned count = entry->stubcount + entry->vstubcount;
        u32 *table = entry->entrytable;
        unsigned i;
        if (bytes < sizeof(*entry) || bytes > mod->ent_size - offset || !table)
            break;
        gobt_log("library %.64s functions=%u variables=%u\n",
                 entry->libname ? entry->libname : "syslib",
                 entry->stubcount, entry->vstubcount);
        for (i = 0; i < count; ++i)
            gobt_log("export %s nid=%08lX address=%08lX kind=%c\n",
                     mod->modname, table[i], table[count + i],
                     i < entry->stubcount ? 'f' : 'v');
        offset += bytes;
    }
}

static void dump_imports(SceModule *mod)
{
    unsigned offset = 0;
    /* Old entries can end before vstubtable; only read function fields. */
    const unsigned minimum = 20;
    while (offset + minimum <= mod->stub_size) {
        SceLibraryStubTable *entry = (void *)((u8 *)mod->stub_top + offset);
        unsigned bytes = entry->len * 4u;
        unsigned i;
        if (bytes < minimum || bytes > mod->stub_size - offset ||
            !entry->nidtable || !entry->stubtable)
            break;
        for (i = 0; i < entry->stubcount; ++i)
            gobt_log("import %.64s nid=%08X stub=%08lX\n",
                     entry->libname ? entry->libname : "?", entry->nidtable[i],
                     (u32)entry->stubtable + 8u * i);
        offset += bytes;
    }
}

static void dump_text(SceModule *mod)
{
    char path[160];
    unsigned done = 0;
    SceUID fd;
    /* Executable text only: no runtime data, pairing database or link keys.
     * UID in filename avoids reusing a relocated image's base address. */
    if (!mod->text_size || mod->text_size > 2u * 1024 * 1024) return;
    /* Fixed root + at most 27 name bytes + UID fits the 160-byte buffer. */
    sprintf(path, ROOT "/%.27s-%08X.text.bin", mod->modname, (unsigned)mod->modid);
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) {
        gobt_log("dump open failed %s error=%08X\n", path, fd);
        return;
    }
    while (done < mod->text_size) {
        unsigned chunk = mod->text_size - done;
        int wrote;
        if (chunk > 16384) chunk = 16384;
        wrote = sceIoWrite(fd, (void *)(mod->text_addr + done), chunk);
        if (wrote <= 0) break;
        done += wrote;
    }
    sceIoClose(fd);
    gobt_log("dump %s bytes=%u expected=%lu\n", path, done, mod->text_size);
}
#endif

#define log_line gobt_log

static void inspect_module(SceModule *mod)
{
    unsigned i;
    int interrupts;
    if (leaving || !mod || !gobt_capture_module(mod->modname)) return;
    interrupts = sceKernelCpuSuspendIntr();
    for (i = 0; i < seen_count; ++i)
        if (seen[i] == mod->modid) {
            sceKernelCpuResumeIntr(interrupts);
            return;
        }
    /* Bounded per-boot capture. Stop writing after 128 distinct modules. */
    if (seen_count == sizeof(seen) / sizeof(seen[0])) {
        sceKernelCpuResumeIntr(interrupts);
        return;
    }
    seen[seen_count++] = mod->modid;
    sceKernelCpuResumeIntr(interrupts);
#if PSPGOBT_DEBUG
    log_line("module %s uid=%08X base=%08lX text=%08lX\n",
             mod->modname, mod->modid, mod->text_addr, mod->text_size);
    dump_exports(mod);
    dump_imports(mod);
    /* Capture before our optional patch, so the image records originals. */
    dump_text(mod);
#endif
    if (!strcmp(mod->modname, "sceBSMan")) {
#if PSPGOBT_DEBUG
        int pair_trace = gobt_install_pair_command_trace(mod, firmware);
        log_line("bt-pair-command trace setup result=%d (0=installed -2=image mismatch)\n",
                 pair_trace);
#endif
        int bridge = gobt_install_input_bridge(mod, firmware);
        log_line("bt-input bridge setup result=%d (0=installed -2=image mismatch -3=provider mismatch -7=local test active)\n",
                 bridge);
    }
    if (!strcmp(mod->modname, "bluetooth_plugin_module")) {
        int hid;
        int trace = gobt_trace_device_info(mod, firmware);
        log_line("device-info trace setup result=%d (0=installed -2=image mismatch -3=provider missing)\n", trace);
        hid = gobt_install_hid_route(mod, firmware);
        log_line("xmb-hid-route setup result=%d (0=installed -2=image mismatch)\n", hid);
        int pair = hid == 0 ? gobt_install_pair_route(mod, firmware) : -1;
        gobt_session_trace(GOBT_TRACE_INSTALL, 0, hid, pair, trace);
        log_line("bt-pair-route setup result=%d (0=installed -2=image mismatch -3=provider mismatch)\n",
                 pair);
        if (enable_filter) {
            int interrupts = sceKernelCpuSuspendIntr();
            int result = gobt_patch_filter(firmware, (u8 *)mod->text_addr, mod->text_size);
            sceKernelCpuResumeIntr(interrupts);
            if (result == 0) {
                sceKernelDcacheWritebackInvalidateAll();
                sceKernelIcacheInvalidateAll();
            }
            log_line("discovery filter patch result=%d (0=applied 1=already -3=instruction mismatch)\n", result);
        } else {
            log_line("discovery filter unchanged; diagnostic mode\n");
        }
    }
}

static int on_module_start(SceModule *mod)
{
    int result = previous ? previous(mod) : 0;
    if (!leaving) inspect_module(mod);
    return result;
}

int module_start(SceSize args, void *argp)
{
    SceUID modules[256], fd;
    int count = 0, i;
    (void)args; (void)argp;
    if (sceKernelGetModel() != PSP_GO) return -1;
    firmware = sceKernelDevkitVersion();
    if (firmware != 0x06060010 && firmware != 0x06060110) return -1;
    gobt_session_trace_start(firmware);
#if PSPGOBT_DEBUG
    sceIoMkdir("ef0:/seplugins", 0777);
    sceIoMkdir(ROOT, 0777);
    log_line("\nPSPGoBT development probe 1.0 firmware=%08lX model=%d\n",
             firmware, sceKernelGetModel());
    log_line("Bluetooth pairing enabled. Input bridge selects supported Sony controllers and validates reports.\n");
    log_line("Device-info tracing observes original results; no registration status is overridden.\n");
    log_line("Guarded Sony controller XMB HID routing patch enabled; native input transport is still under test.\n");
    log_line("Guarded Sony controller full-record registration enabled; Bluetooth pairing remains experimental.\n");
#endif
    fd = sceIoOpen(ROOT "/enable-filter", PSP_O_RDONLY, 0);
    enable_filter = fd >= 0;
    if (fd >= 0) sceIoClose(fd);
    /* Observe new modules before inventory so startup cannot miss a provider
     * loaded while the initial module list is being inspected. */
    previous = sctrlHENSetStartModuleHandler(on_module_start);
    /* Inventory modules already loaded before the plugin. */
    if (sceKernelGetModuleIdList(modules, sizeof(modules), &count) >= 0) {
        if (count > 256) count = 256;
        for (i = 0; i < count; ++i) {
            SceModule *mod = sceKernelFindModuleByUID(modules[i]);
            if (mod) {
                log_line("loaded %s\n", mod->modname);
                inspect_module(mod);
            }
        }
    }
    {
        int result = gobt_start_input_bridge(firmware);
        log_line("bt-input resident worker start result=%d\n", result);
    }
    fd = sceIoOpen(ROOT "/input-test", PSP_O_RDONLY, 0);
    if (fd >= 0) {
        int result;
        sceIoClose(fd);
        result = gobt_sink_start(1);
        log_line("local native DS3 input test start result=%d\n", result);
    } else {
        fd = sceIoOpen(ROOT "/enable-input", PSP_O_RDONLY, 0);
        if (fd >= 0) {
            int result;
            sceIoClose(fd);
            result = gobt_sink_start(0);
            log_line("native DS3 sink start result=%d\n", result);
        }
    }
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    return -1;
}

int module_reboot_before(void *argp, int arg2, int arg3, int arg4)
{
    (void)argp; (void)arg2; (void)arg3; (void)arg4;
    leaving = 1;
    gobt_shutdown_input_bridge(1);
    gobt_sink_shutdown();
    gobt_session_trace_stop();
    return 0;
}

int module_reboot_phase(int arg1, void *argp, int arg3, int arg4)
{
    (void)arg1; (void)argp; (void)arg3; (void)arg4;
    leaving = 1;
    gobt_shutdown_input_bridge(0);
    gobt_sink_shutdown();
    return 0;
}

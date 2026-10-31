#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman_kernel.h>
#include <psputilsforkernel.h>
#include <pspsysevent.h>
#include <systemctrl.h>
#include <stdint.h>
#include <string.h>
#include "bt_input_bridge.h"
#include "session_trace.h"
#include "native_sink.h"
#include "input_state.h"
#include "probe_policy.h"
#include "probe_log.h"
#include "report_bridge.h"
#include "target_cache.h"

typedef int (*ReadCallback)(int, int, void *);
typedef int (*ReadSubmit)(unsigned, void *, unsigned, ReadCallback, void *);
#define RECEIVE_CAPACITY 128u
#define INPUT_BATCH_LIMIT 8u
#define INPUT_IDLE_US 50000u
typedef struct {
    unsigned source, generation, device, size, event;
    int status;
    uint64_t received;
    uint8_t bytes[RECEIVE_CAPACITY];
} ReceivedPacket;
typedef struct {
    unsigned generation, checked, accepted, good, bad, initialized, attempts;
    uint64_t previous, assembly_started, retry_after;
    GoBtReportAssembly assembly;
} InputSource;

static ReadSubmit original_submit;
static ReadCallback original_receive;
static int (*get_info)(unsigned, void *);
static int (*record_command)(const void *, void *, int);
static int (*start_hid)(unsigned);
static int (*stop_hid)(unsigned);
static int (*disconnect_hid)(unsigned, int, int);
static int (*session_event)(const void *);
static int (*finish_disconnect)(const void *, void *);
static int (*native_poll_flags)(int, unsigned, int, unsigned *);
static int (*native_clear_flags)(int, unsigned);
static u32 connect_poll_original, connect_poll_replacement;
static u32 connect_poll_stub[4] __attribute__((aligned(16)));
static int (*native_identity)(unsigned, void *, void *, void *);
static int (*native_register)(void *);
static unsigned (*connection_flags)(unsigned);
static int (*write_hid)(unsigned, const void *, unsigned, unsigned *);
static uintptr_t contexts[2];
static uintptr_t cdc_read_states;
static uintptr_t paired_records;
static GoBtTargetCache target_cache;
static unsigned cache_loaded, cache_revision, cache_dirty;
static uint64_t cache_retry_after;
#define TARGET_CACHE_PATH "ef0:/seplugins/pspgobt/controllers.cfg"
static unsigned generations[2];
static ReceivedPacket queue[16];
static unsigned head, tail, queued, dropped;
static int suspended, active_source = -1;
static unsigned pending_devices, power_generation;
static unsigned checked_devices, linked_devices, serviced_devices;
static uint64_t next_link_poll;
static uint64_t setup_retry_after[9];
static uint64_t stream_started[2], last_report[2];
static unsigned recover_devices, stream_recoveries[9];
static unsigned prepared_devices, discovery_events;
static unsigned device_models[9];
static unsigned discovery_bypassed[2], reads_ready[2], wide_active[2];
static unsigned resetting[2];
static unsigned disconnect_requests, closing_devices, pending_closures;
static unsigned session_epochs[9];
static unsigned close_attempts[9];
static uintptr_t wide_native[2];
static uint8_t receive_buffers[2][RECEIVE_CAPACITY] __attribute__((aligned(64)));
static int installed;
static int worker_started;
static int binding;
static int quiescing, shutdown_done, shutdown_cleanup_done;
static unsigned shutdown_attempted;
static unsigned callback_depth;
static SceUID worker_id = -1, cdc_provider_uid;
static SceUID input_sema = -1;
static uintptr_t hook_base;
static u32 hook_original[8], hook_replacement[8];
static u32 close_original;
static const unsigned hook_sites[] = {
    GOBT_RX_FIRST_CALL_OFFSET, GOBT_RX_NEXT_CALL_OFFSET,
    GOBT_RX_IDENTITY_CALL_OFFSET, GOBT_RX_SERVICE_CALL_OFFSET,
    GOBT_SETUP_EVENT_CALL_OFFSET, GOBT_SETUP_CHECKED_CALL_OFFSET,
    GOBT_SETUP_FLAGS_CALL_OFFSET, GOBT_SESSION_EVENT_CALL_OFFSET
};
static SceUID provider_uid;
static unsigned long bridge_firmware;
static int bind_provider(SceModule *module, unsigned long firmware);
static void callback_enter(void);
static int callback_leave(int result);
static uint8_t output[128] __attribute__((aligned(64)));
static InputSource sources[2];

/* A binary semaphore retains a notification arriving just before the wait.
 * Only wake after the outer Sony callback has finished reposting its read;
 * the higher-priority worker must not interrupt that ownership handoff. */
static void wake_worker(void)
{
    int interrupts = sceKernelCpuSuspendIntr();
    if (input_sema >= 0 && !callback_depth &&
        (queued || pending_devices || pending_closures || quiescing)) {
        unsigned k1 = pspSdkSetK1(0);
        sceKernelSignalSema(input_sema, 1); /* Already signalled is harmless. */
        pspSdkSetK1(k1);
    }
    sceKernelCpuResumeIntr(interrupts);
}

static void wait_input(void)
{
    unsigned timeout = INPUT_IDLE_US;
    if (input_sema >= 0) sceKernelWaitSema(input_sema, 1, &timeout);
    else sceKernelDelayThread(INPUT_IDLE_US);
}

#if PSPGOBT_DEBUG
typedef struct {
    uint64_t next_log;
    unsigned last_flags, emitted, seen, caps, name_matches;
    int info_result;
} DebugDevice;
static DebugDevice debug_devices[9];
#endif

static __attribute__((noipa)) int cached_info(unsigned device, void *destination)
{
    struct { u16 command, bytes; const void *payload; } request;
    struct { u16 expected, bytes, status, reserved; void *payload; } response;
    u16 raw[40] = {0}, slot = device;
    u8 *info = destination;
    int result;
    if (device < 1 || device > 8 || !info || ((const u32 *)info)[0] != 80) return -1;
    /* The public device-info API can send 0x64 and wait for remote refresh
     * before reading this same 0x63 record. Reconnect only needs the stored
     * name/capabilities, not discovery. Leave the public API untouched. */
    request.command = 0x63;
    request.bytes = sizeof(slot);
    request.payload = &slot;
    response.expected = 0x163;
    response.bytes = sizeof(raw);
    response.status = 0xffff;
    response.reserved = 0;
    response.payload = raw;
    result = record_command(&request, &response, 0);
    gobt_log("bt-input cached-record device=%u result=%08X status=%04X\n",
             device, (unsigned)result, response.status);
    if (result < 0) return result;
    if (response.status || (raw[1] && raw[0] != device)) return -1;
    memset(info, 0, 80);
    ((u32 *)info)[0] = 80;
    if (raw[1]) {
        /* Internal classification record only. Omit the unused address tail. */
        memcpy(info + 4, raw, 68);
        memcpy(info + 72, raw + 34, 2);
    }
    return 0;
}

static int source_index(const void *context)
{
    if ((uintptr_t)context == contexts[0]) return 0;
    if ((uintptr_t)context == contexts[1]) return 1;
    return -1;
}

static int kernel_range(uintptr_t pointer, size_t size)
{
    return pointer >= 0x88000000u && pointer <= 0x8c000000u - size;
}

static int session_closing(unsigned device)
{
    return device >= 1 && device <= 8 &&
           ((disconnect_requests | closing_devices) & (1u << device));
}

/* Caller holds interrupts. Do not release buffers here: CDC still owns them
 * until Sony's original local cleanup runs on the worker. */
static void invalidate_session(unsigned device)
{
    unsigned i, bit = 1u << device;
    ++session_epochs[device];
    pending_devices &= ~bit;
    checked_devices &= ~bit;
    serviced_devices &= ~bit;
    recover_devices &= ~bit;
    setup_retry_after[device] = 0;
    stream_recoveries[device] = 0;
    for (i = 0; i < 2; ++i) {
        if (!contexts[i] || ((const u16 *)contexts[i])[2] != device) continue;
        ++generations[i];
        reads_ready[i] = 0;
        stream_started[i] = last_report[i] = 0;
        if (active_source == (int)i) {
            active_source = -1;
            pspgobtDisconnect();
        }
    }
}

unsigned gobt_input_bridge_disconnecting(unsigned device)
{
    unsigned i, token = 0;
    int interrupts;
    if (!installed || quiescing || device < 1 || device > 8) return 0;
    interrupts = sceKernelCpuSuspendIntr();
    for (i = 0; i < 2; ++i)
        if (contexts[i] && discovery_bypassed[i] && ((const u16 *)contexts[i])[2] == device) break;
    if ((prepared_devices & (1u << device)) || i < 2) {
        disconnect_requests |= 1u << device;
        invalidate_session(device);
        token = session_epochs[device];
    }
    sceKernelCpuResumeIntr(interrupts);
    return token;
}

static int native_close_hid(unsigned device)
{
    unsigned i, selected;
    int interrupts;
    callback_enter();
    if (device < 1 || device > 8) return callback_leave(stop_hid(device));
    /* The worker is already past its last provider call. Let Sony finish
     * reboot teardown directly if an outstanding DMA kept hooks resident. */
    if (quiescing && shutdown_cleanup_done) return callback_leave(stop_hid(device));
    interrupts = sceKernelCpuSuspendIntr();
    selected = (prepared_devices | disconnect_requests | closing_devices) & (1u << device);
    for (i = 0; i < 2 && !selected; ++i)
        selected = contexts[i] && discovery_bypassed[i] && ((const u16 *)contexts[i])[2] == device;
    if (selected) {
        closing_devices |= 1u << device;
        pending_closures |= 1u << device;
        close_attempts[device] = 0;
        disconnect_requests &= ~(1u << device);
        prepared_devices &= ~(1u << device);
        invalidate_session(device);
    }
    sceKernelCpuResumeIntr(interrupts);
    /* This verified manager call site ignores the result. Defer only our
     * source, leaving the event thread free to answer in-flight commands.
     * The worker finishes old setup before cancellation and new setup. */
    return callback_leave(selected ? 0 : stop_hid(device));
}

void gobt_input_bridge_disconnect_result(unsigned device, unsigned token, int result)
{
    int interrupts;
    if (!token || device < 1 || device > 8) return;
    interrupts = sceKernelCpuSuspendIntr();
    if (session_epochs[device] == token && result < 0)
        disconnect_requests &= ~(1u << device);
    sceKernelCpuResumeIntr(interrupts);
    /* A successful already-disconnected request may have no further event. */
    if (result >= 0 && installed && !quiescing &&
        session_epochs[device] == token && !(connection_flags(device) & 2))
        native_close_hid(device);
}

int gobt_input_bridge_connect_ready(unsigned device)
{
    unsigned attempts;
    /* XMB syscall thread only, before entering Sony's connection API. The
     * manager thread must remain runnable while the worker drains cleanup. */
    for (attempts = 0; session_closing(device) && !quiescing && attempts < 50; ++attempts)
        sceKernelDelayThread(5000);
    return session_closing(device) || quiescing ? -1 : 0;
}

static void load_target_cache(void)
{
    uint8_t bytes[GOBT_TARGET_CACHE_BYTES + 1];
    SceUID fd;
    int size;
    if (cache_loaded) return;
    cache_loaded = 1;
    fd = sceIoOpen(TARGET_CACHE_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    size = sceIoRead(fd, bytes, sizeof(bytes));
    sceIoClose(fd);
    if (size >= 0) gobt_target_decode(&target_cache, bytes, (size_t)size);
}

static void save_target_cache(void)
{
    uint8_t bytes[GOBT_TARGET_CACHE_BYTES];
    unsigned revision;
    SceUID fd;
    int interrupts, result, closed;
    if (!cache_dirty || (uint64_t)sceKernelGetSystemTimeWide() < cache_retry_after) return;
    interrupts = sceKernelCpuSuspendIntr();
    revision = cache_revision;
    result = gobt_target_encode(&target_cache, bytes);
    sceKernelCpuResumeIntr(interrupts);
    if (result < 0) return;
    cache_retry_after = sceKernelGetSystemTimeWide() + 1000000;
    fd = sceIoOpen(TARGET_CACHE_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) return;
    result = sceIoWrite(fd, bytes, sizeof(bytes));
    closed = sceIoClose(fd);
    if (result != (int)sizeof(bytes) || closed < 0) return;
    interrupts = sceKernelCpuSuspendIntr();
    if (revision == cache_revision) cache_dirty = 0;
    sceKernelCpuResumeIntr(interrupts);
}

static int slot_address(unsigned device, uint8_t address[6])
{
    const u16 *record;
    int interrupts, valid;
    if (!installed || device < 1 || device > 8 ||
        !kernel_range(paired_records, 0xa0)) return 0;
    record = (const u16 *)(paired_records + 0x14 + device * 16);
    interrupts = sceKernelCpuSuspendIntr();
    valid = record[0] == device && record[1] == 0x0402;
    if (valid) memcpy(address, (const uint8_t *)record + 6, 6);
    sceKernelCpuResumeIntr(interrupts);
    return valid;
}

static int classify(unsigned device, void *info, int *query, int *cached)
{
    uint8_t address[6];
    int has_address, selected, interrupts;
    *cached = 0;
    *query = -1;
    if (!installed || suspended || quiescing || device < 1 || device > 8 || session_closing(device)) return 0;
    has_address = slot_address(device, address);
    interrupts = sceKernelCpuSuspendIntr();
    if (has_address) *cached = gobt_target_contains(&target_cache, address);
    sceKernelCpuResumeIntr(interrupts);
    if (*cached) {
        /* The local 0x63 record avoids a remote DID query on reconnect. */
        *query = record_command ? cached_info(device, info) : -1;
        device_models[device] = *query >= 0 && gobt_input_metadata(info, 80) ?
                                gobt_controller_model(info, 80, 80) : GOBT_MODEL_NONE;
        return 1;
    }
    *query = get_info(device, info);
    selected = *query >= 0 && gobt_input_metadata(info, 80);
    device_models[device] = selected ? gobt_controller_model(info, 80, 80) : GOBT_MODEL_NONE;
    if (selected && has_address) {
        uint8_t current[6];
        if (!slot_address(device, current) || memcmp(address, current, 6)) return 0;
        interrupts = sceKernelCpuSuspendIntr();
        if (gobt_target_remember(&target_cache, address) == 1) {
            ++cache_revision;
            cache_dirty = 1;
        }
        sceKernelCpuResumeIntr(interrupts);
    }
    return selected;
}

static int bridge_target(unsigned device)
{
    unsigned i;
    uint8_t address[6];
    if (prepared_devices & (1u << device)) return 1;
    for (i = 0; i < 2; ++i)
        if (contexts[i] && discovery_bypassed[i] && ((const u16 *)contexts[i])[2] == device) return 1;
    return slot_address(device, address) && gobt_target_contains(&target_cache, address);
}

int gobt_input_bridge_known(unsigned device)
{
    int interrupts, selected;
    uint8_t address[6];
    if (device < 1 || device > 8) return 0;
    interrupts = sceKernelCpuSuspendIntr();
    selected = installed && !quiescing && slot_address(device, address) &&
               gobt_target_contains(&target_cache, address);
    sceKernelCpuResumeIntr(interrupts);
    return selected;
}

/* The native Connect helper reserves a free entry before this poll. Reusing
 * its event UID can retain the previous connection's success/failure bits.
 * Only reset a selected generic HID entry marked closed (bit 2), without an
 * incoming-operation busy bit or a real connected HID mask. Never alter the
 * later wait, where bit 4 means a failure of the NEW radio request. */
static int native_connect_poll(int uid, unsigned bits, int mode, unsigned *flags,
                               const u16 *entry, unsigned device)
{
    uintptr_t pointer = (uintptr_t)entry, first = paired_records + 0xe4;
    unsigned before = 0, after = 0;
    int cleared = 0, result, interrupts;
    callback_enter();
    interrupts = sceKernelCpuSuspendIntr();
    result = native_poll_flags(uid, bits, mode, flags);
    if (result >= 0 && installed && !quiescing && device >= 1 && device <= 8 &&
        bits == ~0u && mode == 1 && flags && pointer >= first && pointer - first < 48 &&
        !((pointer - first) & 7) && kernel_range(pointer, 8) && entry[0] == device &&
        entry[1] == 0x0202 && ((const u32 *)entry)[1] == (u32)uid &&
        !(*(const u16 *)(paired_records + 0x9c + device * 8) & 2) && bridge_target(device)) {
        before = *flags;
        if ((before & 2) && (before & 5) && !(before & 8)) {
            result = native_clear_flags(uid, ~5u); /* preserve close bit 2 and other flags */
            if (result >= 0) {
                cleared = 1;
                result = native_poll_flags(uid, bits, mode, flags);
                if (result >= 0) after = *flags;
            }
        }
    }
    sceKernelCpuResumeIntr(interrupts);
    if (cleared) gobt_session_trace(GOBT_TRACE_CONNECT_FLAGS, device, before, after, result);
    return callback_leave(result);
}

static int native_session_event(const u16 *event)
{
    unsigned device = event[0], state = event[1], i;
    u16 *entry = NULL;
    int interrupts;
    callback_enter();
    gobt_session_trace(GOBT_TRACE_EVENT, device, state, event[2], disconnect_requests | closing_devices);
    if (!installed || !paired_records || device < 1 || device > 8 ||
        (state != 0 && state != 2 && state != 3) || (event[2] & 0xff) != 2 || !bridge_target(device))
        return callback_leave(session_event(event));
    if (state == 0) {
        for (i = 0; i < 2; ++i)
            if (contexts[i] && discovery_bypassed[i] && ((const u16 *)contexts[i])[2] == device) break;
        if (!quiescing && (session_closing(device) || i < 2)) {
            /* A new real connection supersedes an old close request. Retire
             * the old local read before setup; do not disconnect the new link. */
            native_close_hid(device);
        }
        return callback_leave(session_event(event));
    }
    /* Match the same native active entry as Sony's event dispatcher. Do not
     * create an entry or infer a disconnect from an idle input stream. */
    for (i = 0; i < 6; ++i) {
        u16 *candidate = (u16 *)(paired_records + 0xe4 + i * 8);
        if (candidate[0] == device && (candidate[1] & 0xff) == 2) { entry = candidate; break; }
    }
    if (!entry) return callback_leave(session_event(event));
    if (state == 3) {
        interrupts = sceKernelCpuSuspendIntr();
        disconnect_requests |= 1u << device;
        invalidate_session(device);
        sceKernelCpuResumeIntr(interrupts);
        /* This is the real BSM close indication. Sony normally gates its
         * finalizer on event-flag bit 8; the bypassed generic HID service is
         * not there to complete that intermediate handshake. Use Sony's
         * original finalizer with this exact event and matching live entry:
         * it clears manager flags, notifies XMB and invokes local cleanup. */
        int result = finish_disconnect(event, entry);
#if PSPGOBT_SESSION_TRACE
        gobt_session_trace(GOBT_TRACE_EVENT_DONE, device, state, (unsigned)result, connection_flags(device));
#endif
        return callback_leave(result);
    }
    /* State 2 also wakes a failed outgoing Connect (wait mask 1|4).
     * It is not evidence that the HID service must issue another Disconnect.
     * In particular, never put Sony's unbounded 0x69 completion wait on the
     * input worker or latch a closing session from this indication alone. */
    return callback_leave(session_event(event));
}

void gobt_input_bridge_connected(unsigned device)
{
    int interrupts;
    if (!installed || quiescing || device < 1 || device > 8 || session_closing(device)) return;
    interrupts = sceKernelCpuSuspendIntr();
    if (!suspended) pending_devices |= 1u << device;
    sceKernelCpuResumeIntr(interrupts);
    wake_worker();
}

static int schedule_setup(unsigned device, int checked)
{
    int interrupts;
    if (quiescing) return -1;
    if (!installed || device < 1 || device > 8) return start_hid(device);
    interrupts = sceKernelCpuSuspendIntr();
    if (!suspended && !session_closing(device)) {
        pending_devices |= 1u << device;
        if (checked) checked_devices |= 1u << device;
    }
    sceKernelCpuResumeIntr(interrupts);
    wake_worker();
    /* Both verified native callers can continue without a context: one
     * ignores the result, the other disconnects on failure. The worker
     * performs that same checked failure cleanup after real setup returns.
     * This leaves the manager event thread free to answer the metadata API. */
    return 0;
}

static int native_setup_event(unsigned device) { return schedule_setup(device, 0); }
static int native_setup_checked(unsigned device) { return schedule_setup(device, 1); }

int gobt_input_bridge_prepare(unsigned device)
{
    u32 info[20] = {80};
    unsigned k1, epoch;
    int result, selected, cached, interrupts;
    if (!installed || device < 1 || device > 8 || suspended || quiescing || session_closing(device)) return -1;
    epoch = power_generation;
    k1 = pspSdkSetK1(0);
    gobt_session_trace(GOBT_TRACE_PREPARE, device, epoch, prepared_devices, 0);
    selected = classify(device, info, &result, &cached);
    gobt_session_trace(GOBT_TRACE_PREPARED, device, result, selected, cached);
#if PSPGOBT_DEBUG
    {
        static const char prefix[] = "DualSense";
        const u16 *record = (const u16 *)info;
        DebugDevice *debug = &debug_devices[device];
        unsigned i, name_matches = 1;
        for (i = 0; i < sizeof(prefix) - 1; ++i)
            if (record[3 + i] != (unsigned char)prefix[i]) name_matches = 0;
        if (record[12] && record[12] != ' ') name_matches = 0;
        debug->seen = 1;
        debug->info_result = result;
        debug->caps = cached ? 0x0402 : record[36];
        debug->name_matches = name_matches;
        gobt_log("bt-input stored-record device=%u result=%08X size80=%u name-match=%u caps=%04X metadata=%u cached-target=%u\n",
                 device, (unsigned)result, info[0] == 80, name_matches, debug->caps, selected, cached);
    }
#endif
    interrupts = sceKernelCpuSuspendIntr();
    prepared_devices &= ~(1u << device);
    if (selected && !suspended && epoch == power_generation && !session_closing(device))
        prepared_devices |= 1u << device;
    sceKernelCpuResumeIntr(interrupts);
    gobt_log("bt-input prepare device=%u info=%08X metadata=%u\n", device, (unsigned)result, selected);
    pspSdkSetK1(k1);
    return suspended || epoch != power_generation || session_closing(device) ? -1 : result < 0 ? result : selected;
}

static unsigned discovery_ticket(void)
{
    int interrupts = sceKernelCpuSuspendIntr();
    unsigned event = discovery_events < 16 ? ++discovery_events : 0;
    sceKernelCpuResumeIntr(interrupts);
    return event;
}

static int prepared_connected(unsigned device)
{
    unsigned epoch = power_generation;
    return installed && !suspended && !session_closing(device) && device >= 1 && device <= 8 &&
           (prepared_devices & (1u << device)) && (connection_flags(device) & 2) &&
           !suspended && epoch == power_generation;
}

static unsigned native_setup_flags(unsigned device)
{
    unsigned epoch = power_generation;
    unsigned flags = connection_flags(device);
    /* Keep the real link state intact. Only the local HID constructor's
     * branch selection changes for an already selected, connected DualSense. */
    if (installed && !suspended && device >= 1 && device <= 8 &&
        (prepared_devices & (1u << device)) && (flags & 2) &&
        epoch == power_generation)
        return (flags & ~0x400u) | 0x200u;
    return flags;
}

static int trace_native_identity(unsigned device, void *vendor, void *product, void *extra)
{
    unsigned event = discovery_ticket();
    unsigned k1 = pspSdkSetK1(0);
    unsigned i;
    int result;
    i = 2;
    if (prepared_connected(device))
        for (i = 0; i < 2; ++i)
            if (((const u16 *)contexts[i])[2] == device) break;
    if (i < 2 && vendor && product && !extra &&
        !(((uintptr_t)vendor | (uintptr_t)product) & 1)) {
        /* This one internal setup call only distinguishes native DS3 IDs.
         * The caller ignores its return and compares these two words. Avoid
         * remote DID discovery entirely for our prepared DualSense path;
         * zero means unknown, preventing entry into Sony's DS3-only setup.
         * -1 explicitly means not queried here, not a firmware error result.
         * Public DID APIs and all other devices retain their original calls. */
        *(u16 *)vendor = 0;
        *(u16 *)product = 0;
        if (event) gobt_log("bt-input native DID skipped device=%u source=%u event=%u; prepared Sony controller, IDs unknown\n",
                            device, i, event);
        pspSdkSetK1(k1);
        return -1;
    }
    if (event) gobt_log("bt-input native DID begin device=%u event=%u\n", device, event);
    pspSdkSetK1(k1);
    result = native_identity(device, vendor, product, extra);
    k1 = pspSdkSetK1(0);
    if (event) gobt_log("bt-input native DID end device=%u event=%u result=%08X\n",
                        device, event, (unsigned)result);
    pspSdkSetK1(k1);
    return result;
}

static __attribute__((noipa)) int register_hid_service(void *descriptor)
{
    unsigned device = 0, bypass = 0, event = discovery_ticket();
    unsigned k1 = pspSdkSetK1(0);
    int index = source_index((const void *)((uintptr_t)descriptor - 8));
    int result;
    if (index >= 0) {
        device = ((const u16 *)contexts[index])[2];
        bypass = prepared_connected(device);
        discovery_bypassed[index] = bypass != 0;
    }
    if (event) gobt_log("bt-input HID discovery begin device=%u source=%d bypass=%u event=%u\n",
                        device, index, bypass != 0, event);
    pspSdkSetK1(k1);
    /* The native service performs synchronous DID/descriptor discovery before
     * the first CDC read. Our worker already parses DualSense reports and
     * feeds the external controller port. Skip only that service registration
     * for a connected, preselected DualSense; retain native buffer allocation,
     * read submission, callback, queues, and teardown. Input acceptance still
     * requires the separate connected-source selection in identify(). */
    result = bypass ? 0 : native_register(descriptor);
    k1 = pspSdkSetK1(0);
    if (event) gobt_log("bt-input HID discovery end device=%u bypass=%u event=%u result=%08X\n",
                        device, bypass != 0, event, (unsigned)result);
    pspSdkSetK1(k1);
    return result;
}

static void capture(int index, int size, const uint8_t *bytes, unsigned device, unsigned event)
{
    ReceivedPacket *packet;
    int interrupts = sceKernelCpuSuspendIntr();
    if (suspended || quiescing || session_closing(device) || queued == 16) {
        if (!suspended) ++dropped;
        sceKernelCpuResumeIntr(interrupts);
        return;
    }
#if PSPGOBT_SESSION_TRACE
    static unsigned traced_generation[2];
    if (event || traced_generation[index] != generations[index]) {
        gobt_session_trace(GOBT_TRACE_READ, device, event, (unsigned)size, generations[index]);
        if (!event) traced_generation[index] = generations[index];
    }
#endif
    packet = &queue[tail];
    packet->source = (unsigned)index;
    packet->generation = generations[index];
    packet->device = device;
    packet->event = event;
    packet->status = size;
    packet->size = size > 0 ? (unsigned)size : 0;
    packet->received = sceKernelGetSystemTimeWide();
    if (size > 0 && size <= (int)RECEIVE_CAPACITY && bytes)
        memcpy(packet->bytes, bytes, (unsigned)size);
    tail = (tail + 1) % 16;
    ++queued;
    sceKernelCpuResumeIntr(interrupts);
    if (!callback_depth) wake_worker();
}

static void callback_enter(void)
{
    int interrupts = sceKernelCpuSuspendIntr();
    ++callback_depth;
    sceKernelCpuResumeIntr(interrupts);
}

static int callback_leave(int result)
{
    int interrupts = sceKernelCpuSuspendIntr();
    --callback_depth;
    sceKernelCpuResumeIntr(interrupts);
    wake_worker();
    return result;
}

static int receive_input(int channel, int size, void *context)
{
    int index = source_index(context);
    if (index < 0) return -1;
    callback_enter();
    if (index >= 0) {
        unsigned device = ((const u16 *)context)[2];
        uintptr_t entry = ((const u32 *)context)[10];
        if (size <= 0) {
            int interrupts = sceKernelCpuSuspendIntr();
            ++generations[index];
            reads_ready[index] = 0;
            serviced_devices &= ~(1u << device);
            sceKernelCpuResumeIntr(interrupts);
            capture(index, size, NULL, device, 2);
        } else if (kernel_range(entry, 12) && !(entry & 3)) {
            uintptr_t data = ((const u32 *)entry)[2];
            if (size > 64) capture(index, size, NULL, device, 3);
            else if (kernel_range(data, (unsigned)size))
                capture(index, size, (const uint8_t *)data, device, 0);
        }
    }
    /* Sony retains ownership of the read buffer and all original HID queues.
     * Only a bounded copy enters our worker; no file I/O or parsing here. */
    return callback_leave(original_receive(channel, size, context));
}

static int receive_wide(int buffer, int size, void *context)
{
    int index = source_index(context), native_size = size;
    uintptr_t data, entry;
    unsigned device;
    callback_enter();
    if (index < 0 || !wide_active[index] ||
        (uintptr_t)(unsigned)buffer != (uintptr_t)receive_buffers[index]) return callback_leave(-1);
    data = wide_native[index];
    entry = ((const u32 *)context)[10];
    if (!kernel_range(entry, 12) || (entry & 3) ||
        ((const u32 *)entry)[2] != data || !kernel_range(data, 64)) return callback_leave(-1);
    device = ((const u16 *)context)[2];
    wide_active[index] = 0;
    if (size > 0 && size <= (int)RECEIVE_CAPACITY) {
        /* Copy the complete HIDP packet for our decoder before Sony requeues.
         * Its entry remains exactly 64 bytes: only a bounded compatibility
         * prefix reaches its queue, with a correspondingly bounded length.
         * Native descriptor parsing was bypassed for this source. */
        capture(index, size, receive_buffers[index], device, 0);
        native_size = size > 64 ? 64 : size;
        memcpy((void *)data, receive_buffers[index], (unsigned)native_size);
    } else {
        int interrupts = sceKernelCpuSuspendIntr();
        ++generations[index];
        reads_ready[index] = 0;
        serviced_devices &= ~(1u << device);
        sceKernelCpuResumeIntr(interrupts);
        capture(index, size, NULL, device, 2);
        if (size > (int)RECEIVE_CAPACITY) native_size = -1;
    }
    /* CDC callbacks receive the buffer pointer, not the logical channel.
     * Restore the native pointer while chaining Sony's ownership/read loop. */
    return callback_leave(original_receive((int)data, native_size, context));
}

static int submit(unsigned channel, void *buffer, unsigned size, ReadCallback callback,
                   void *context, int first)
{
    int index = source_index(context);
    int wrapped = callback == original_receive && index >= 0 && size == 64;
    unsigned generation = 0;
    int wide = 0;
    int result;
    /* Sony's cancellation callback normally attempts to requeue. Let its
     * normal error cleanup run, but do not start another DMA during reset. */
    if (wrapped && (resetting[index] || quiescing ||
        session_closing(((const u16 *)context)[2]))) return -1;
    if (wrapped) {
        if (first) {
            int interrupts = sceKernelCpuSuspendIntr();
            ++generations[index];
            reads_ready[index] = 0;
            sceKernelCpuResumeIntr(interrupts);
        }
        callback = receive_input;
        generation = generations[index];
        if (discovery_bypassed[index] && installed && !suspended) {
            uintptr_t entry = ((const u32 *)context)[10];
            if (kernel_range(entry, 12) && !(entry & 3) &&
                ((const u32 *)entry)[2] == (uintptr_t)buffer &&
                kernel_range((uintptr_t)buffer, 64) && !((uintptr_t)buffer & 63)) {
                wide_native[index] = (uintptr_t)buffer;
                wide_active[index] = 1;
                buffer = receive_buffers[index];
                size = RECEIVE_CAPACITY;
                callback = receive_wide;
                wide = 1;
            }
        }
    }
    result = original_submit(channel, buffer, size, callback, context);
    if (wrapped) {
        if (result < 0) {
            int interrupts = sceKernelCpuSuspendIntr();
            reads_ready[index] = 0;
            serviced_devices &= ~(1u << ((const u16 *)context)[2]);
            if (wide) wide_active[index] = 0;
            if (!first) ++generations[index];
            sceKernelCpuResumeIntr(interrupts);
            if (!first) capture(index, result, NULL, ((const u16 *)context)[2], 2);
        } else if (generations[index] == generation) reads_ready[index] = 1;
    }
    if (wrapped && first) {
        unsigned event = discovery_ticket(), k1 = pspSdkSetK1(0);
        if (event)
            gobt_log("bt-input receive-submit source=%d capacity=%u result=%08X generation=%u\n",
                     index, size, (unsigned)result, generations[index]);
        pspSdkSetK1(k1);
        capture(index, result, NULL, ((const u16 *)context)[2], result >= 0 ? 1 : 2);
    }
    return result;
}

static int submit_first(unsigned channel, void *buffer, unsigned size,
                         ReadCallback callback, void *context)
{
    return submit(channel, buffer, size, callback, context, 1);
}

static int submit_next(unsigned channel, void *buffer, unsigned size,
                        ReadCallback callback, void *context)
{
    return submit(channel, buffer, size, callback, context, 0);
}

static __attribute__((noipa)) int identify(unsigned device)
{
    u32 info[20] = {80};
    unsigned epoch = power_generation;
    unsigned k1 = pspSdkSetK1(0);
    int query, cached;
    int selected = classify(device, info, &query, &cached) &&
                   installed && !suspended && (connection_flags(device) & 2) &&
                   epoch == power_generation;
    /* A new PRX or power epoch has no prepared mask yet. Cached metadata and
     * the real connected flag suffice; never fall into remote DID discovery. */
    gobt_log("bt-input identity device=%u info=%08X DID=not-queried selection=%u\n",
             device, (unsigned)query, selected ? 3 : 0);
    pspSdkSetK1(k1);
    return selected ? 3 : 0;
}

static int reset_input_context(unsigned index, unsigned device, int stopping)
{
    const u32 *context = (const u32 *)contexts[index];
    u32 *slot;
    unsigned epoch = power_generation;
    int result, interrupts;
    if ((!stopping && !prepared_connected(device)) ||
        (stopping && (!installed || !discovery_bypassed[index])) || context[0] != 2 ||
        ((const u16 *)context)[2] != device) {
        gobt_session_trace(GOBT_TRACE_RESET_BLOCKED, device, 1, context[0], stopping);
        return -1;
    }
    slot = (u32 *)(cdc_read_states + 2 * 12 + 0x7c);
    interrupts = sceKernelCpuSuspendIntr();
    /* Never cancel a channel held by another device or callback. This also
     * handles a failed submit that left our callback installed but no read. */
    if (slot[0] && (slot[2] != contexts[index] ||
        (slot[0] != (u32)original_receive && slot[0] != (u32)receive_input &&
         slot[0] != (u32)receive_wide))) {
        gobt_session_trace(GOBT_TRACE_RESET_BLOCKED, device, 2,
                           slot[2] == contexts[index], slot[0] == (u32)original_receive);
        sceKernelCpuResumeIntr(interrupts);
        return -1;
    }
    ++generations[index];
    resetting[index] = 1;
    reads_ready[index] = 0;
    serviced_devices &= ~(1u << device);
    if (active_source == (int)index) {
        active_source = -1;
        pspgobtDisconnect();
    }
    sceKernelCpuResumeIntr(interrupts);
    /* Use Sony's own cancel/queue cleanup, never an old entry pointer and
     * never the Bluetooth disconnect helper. Cancellation may run callbacks. */
    gobt_session_trace(GOBT_TRACE_CLEANUP, device, 0, session_epochs[device], closing_devices);
    result = stop_hid(device);
    gobt_session_trace(GOBT_TRACE_CLEANUP_DONE, device, (unsigned)result, session_epochs[device], closing_devices);
    interrupts = sceKernelCpuSuspendIntr();
    ++generations[index];
    resetting[index] = reads_ready[index] = 0;
    if (result >= 0 && ((const u16 *)context)[2] == 0) {
        wide_active[index] = discovery_bypassed[index] = 0;
        wide_native[index] = 0;
    }
    memset(&sources[index], 0, sizeof(sources[index]));
    stream_started[index] = last_report[index] = 0;
    sceKernelCpuResumeIntr(interrupts);
    gobt_log("bt-input local reset device=%u source=%u result=%08X; Bluetooth link preserved\n",
             device, index, (unsigned)result);
    if (stopping) return result < 0 || ((const u16 *)context)[2] != 0 ? -1 : 0;
    return result < 0 || suspended || epoch != power_generation ||
           !(connection_flags(device) & 2) || ((const u16 *)context)[2] != 0 ? -1 : 0;
}

static void finish_closures(void)
{
    unsigned device, i;
    if ((suspended && !quiescing) || callback_depth) return;
    for (device = 1; device <= 8; ++device) {
        unsigned bit = 1u << device, session, indices = 0;
        int result, interrupts = sceKernelCpuSuspendIntr();
        if (!(pending_closures & bit) ||
            (uint64_t)sceKernelGetSystemTimeWide() < setup_retry_after[device]) {
            sceKernelCpuResumeIntr(interrupts);
            continue;
        }
        pending_closures &= ~bit;
        session = session_epochs[device];
        ++close_attempts[device];
        for (i = 0; i < 2; ++i)
            if (((const u16 *)contexts[i])[2] == device) {
                resetting[i] = 1;
                indices |= 1u << i;
            }
        sceKernelCpuResumeIntr(interrupts);
        gobt_session_trace(GOBT_TRACE_CLEANUP, device, 0, session_epochs[device], closing_devices);
        result = stop_hid(device);
        gobt_session_trace(GOBT_TRACE_CLEANUP_DONE, device, (unsigned)result, session_epochs[device], closing_devices);
        interrupts = sceKernelCpuSuspendIntr();
        for (i = 0; i < 2; ++i) {
            /* Sony clears channel/device on success but leaves old queue
             * pointers in the record. Never dereference those after stop. */
            if (!(indices & (1u << i))) continue;
            resetting[i] = 0;
            ++generations[i];
            reads_ready[i] = 0;
            if (result >= 0 && ((const u16 *)contexts[i])[2] == 0) {
                wide_active[i] = discovery_bypassed[i] = 0;
                wide_native[i] = 0;
                memset(&sources[i], 0, sizeof(sources[i]));
            } else result = -1;
        }
        if (session == session_epochs[device]) {
            if (result >= 0) {
                closing_devices &= ~bit;
                disconnect_requests &= ~bit;
                prepared_devices &= ~bit;
                serviced_devices &= ~bit;
                setup_retry_after[device] = 0;
            } else if (close_attempts[device] < 3) {
                setup_retry_after[device] = sceKernelGetSystemTimeWide() + 1000000;
                pending_closures |= bit;
            }
        }
        sceKernelCpuResumeIntr(interrupts);
        gobt_log("bt-input session close device=%u result=%08X attempt=%u\n",
                 device, (unsigned)result, close_attempts[device]);
    }
}

static __attribute__((noipa)) int attach_pending_read(unsigned index, unsigned device)
{
    const u32 *context = (const u32 *)contexts[index];
    uintptr_t entry = context[10], data = 0;
    unsigned channel = context[0], pending = 0, adopted = 0, context_match, buffer_match, ready;
    u32 *slot;
    int interrupts;
    if (session_closing(device) || channel < 1 || channel > 4 || ((const u16 *)context)[2] != device) return 0;
    if (kernel_range(entry, 12) && !(entry & 3)) data = ((const u32 *)entry)[2];
    slot = (u32 *)(cdc_read_states + channel * 12 + 0x7c);
    interrupts = sceKernelCpuSuspendIntr();
    if (slot[0] == (u32)original_receive) pending = 1;
    else if (slot[0] == (u32)receive_input || slot[0] == (u32)receive_wide) pending = 2;
    else if (slot[0]) pending = 3;
    context_match = slot[2] == contexts[index];
    buffer_match = slot[1] == data || (slot[0] == (u32)receive_wide &&
                   wide_active[index] && slot[1] == (u32)receive_buffers[index] &&
                   wide_native[index] == data);
    if (pending == 1 && context_match && buffer_match &&
        kernel_range(data, 64) && !(data & 63)) {
        /* The driver already owns this buffer and pending request. Replace
         * only its callback; the same original callback is still chained by
         * receive_input. Never submit into an entry that may have been freed. */
        ++generations[index];
        slot[0] = (u32)receive_input;
        adopted = 1;
    }
    ready = adopted || (pending == 2 && context_match && buffer_match);
    if (ready) reads_ready[index] = 1;
    sceKernelCpuResumeIntr(interrupts);
    gobt_log("bt-input pending-read device=%u source=%u channel=%u native-mode=%u pending=%u entry=%u context-match=%u buffer-match=%u adopted=%u\n",
             device, index, channel, ((const u16 *)context)[3], pending,
             entry != 0, context_match, buffer_match, adopted);
    return ready;
}

static void initialize(unsigned index, unsigned device, InputSource *source)
{
    unsigned generation = generations[index];
    unsigned channel = *(const u32 *)contexts[index];
    unsigned timeout = 200000;
    int result;
    if (source->initialized || !source->accepted || !device_models[device] ||
        !reads_ready[index] || suspended || quiescing || session_closing(device) ||
        source->attempts >= 3 || (uint64_t)sceKernelGetSystemTimeWide() < source->retry_after ||
        channel < 1 || channel > 4 || ((const u16 *)contexts[index])[2] != device ||
        !(connection_flags(device) & 2)) return;
    if (!stream_started[index]) stream_started[index] = sceKernelGetSystemTimeWide();
    ++source->attempts;
    memset(output, 0, sizeof(output));
    output[0] = 0xa2;
    if (device_models[device] == GOBT_MODEL_DUALSHOCK4) ds4_build_enable(output + 1);
    else ds_build_enable(output + 1, 0);
    gobt_session_trace(GOBT_TRACE_INIT, device, index, generation, source->attempts);
    result = write_hid(channel, output, 79, &timeout);
    gobt_session_trace(GOBT_TRACE_INIT_DONE, device, (unsigned)result, generation, generations[index]);
    if (result == 79 && generations[index] == generation && !session_closing(device)) source->initialized = 1;
    else source->retry_after = sceKernelGetSystemTimeWide() + 200000;
    gobt_log("bt-input init device=%u source=%u channel=%u bytes=79 result=%08X generation=%u attempt=%u\n",
             device, index, channel, (unsigned)result, generation, source->attempts);
}

static __attribute__((noipa)) void connected(unsigned device, int checked)
{
    unsigned i, epoch = power_generation, session = session_epochs[device];
    unsigned flags = connection_flags(device);
    int result, selected, interrupts;
    gobt_log("bt-input connected device=%u flags=%04X; checking HID report setup\n", device, flags);
    if (!(flags & 2) || suspended || quiescing || session_closing(device)) return;
    if ((uint64_t)sceKernelGetSystemTimeWide() < setup_retry_after[device]) return;
    /* Incoming reconnects and boot-time connections never visit the XMB
     * Connect syscall. Read the existing paired record here before native
     * setup can enter remote DID discovery. No pairing record is changed. */
    result = gobt_input_bridge_prepare(device);
    if (result < 0) return; /* Retry from the link poll; never query DID on missing metadata. */
    selected = result == 1;
    if (suspended || epoch != power_generation || session != session_epochs[device] ||
        session_closing(device) || !(connection_flags(device) & 2)) return;
    for (i = 0; i < 2; ++i)
        if (((const u16 *)contexts[i])[2] == device) break;
    if (selected && i < 2 && ((recover_devices & (1u << device)) ||
        !discovery_bypassed[i] || !attach_pending_read(i, device))) {
        /* A surviving Sony context is not proof of a live read. Rebuild a
         * previous instance's context, or recover a stopped owned read. */
        setup_retry_after[device] = sceKernelGetSystemTimeWide() + 1000000;
        interrupts = sceKernelCpuSuspendIntr();
        if (recover_devices & (1u << device)) ++stream_recoveries[device];
        recover_devices &= ~(1u << device);
        sceKernelCpuResumeIntr(interrupts);
        if (reset_input_context(i, device, 0) < 0) return;
        if (suspended || epoch != power_generation || session != session_epochs[device] ||
            session_closing(device) || !(connection_flags(device) & 2)) return;
        i = 2;
    }
    if (i == 2) {
        /* The original native routine still handles other HID devices. All
         * setup runs on this worker so incoming connection events cannot
         * race the paired-record preparation or create duplicate contexts. */
        gobt_session_trace(GOBT_TRACE_SETUP, device, flags, session, selected);
        result = start_hid(device);
        gobt_session_trace(GOBT_TRACE_SETUP_DONE, device, (unsigned)result, session_epochs[device], prepared_devices);
        if (suspended || quiescing || epoch != power_generation ||
            session != session_epochs[device] || session_closing(device)) return;
        gobt_log("bt-input native HID setup device=%u result=%08X\n", device, (unsigned)result);
        if (result < 0) {
            if (selected) setup_retry_after[device] = sceKernelGetSystemTimeWide() + 1000000;
            if (!selected && checked && !suspended && epoch == power_generation && (connection_flags(device) & 2))
                disconnect_hid(device, 2, 0);
            interrupts = sceKernelCpuSuspendIntr();
            checked_devices &= ~(1u << device);
            sceKernelCpuResumeIntr(interrupts);
            return;
        }
        for (i = 0; i < 2; ++i)
            if (((const u16 *)contexts[i])[2] == device) break;
    }
    if (!prepared_connected(device)) {
        interrupts = sceKernelCpuSuspendIntr();
        serviced_devices |= 1u << device;
        checked_devices &= ~(1u << device);
        sceKernelCpuResumeIntr(interrupts);
        return; /* Original setup handles other HID devices. */
    }
    if (!identify(device) || suspended || epoch != power_generation ||
        session != session_epochs[device] || session_closing(device)) return;
    if (i < 2) {
        InputSource *source = &sources[i];
        int ready = attach_pending_read(i, device);
        interrupts = sceKernelCpuSuspendIntr();
        if (ready && reads_ready[i] && ((const u16 *)contexts[i])[2] == device)
            serviced_devices |= 1u << device;
        checked_devices &= ~(1u << device);
        sceKernelCpuResumeIntr(interrupts);
        if (source->generation != generations[i]) {
            if (active_source == (int)i) {
                active_source = -1;
                pspgobtDisconnect();
            }
            memset(source, 0, sizeof(*source));
            stream_started[i] = last_report[i] = 0;
            source->generation = generations[i];
        }
        source->checked = source->accepted = 1;
        initialize(i, device, source);
    } else gobt_log("bt-input device=%u has no readable HID context\n", device);
}

#if PSPGOBT_DEBUG
/* Passive snapshots only: no device queries, remote discovery, read submits,
 * controller writes, or native buffer dereferences. One line per second,
 * capped per link state so a failed stored connection stays inspectable. */
static void debug_link_snapshot(unsigned device, unsigned flags)
{
    DebugDevice *debug = &debug_devices[device];
    InputSource *source = NULL;
    uint64_t now;
    unsigned i, channel = 0, mode = 0, entry = 0, callback = 0, context_match = 0;
    int index = -1;
    if (flags != debug->last_flags) {
        debug->last_flags = flags;
        debug->next_log = 0;
        debug->emitted = 0;
    }
    for (i = 0; i < 2; ++i) {
        const u32 *context = (const u32 *)contexts[i];
        if (((const u16 *)context)[2] != device) continue;
        index = (int)i;
        source = &sources[i];
        channel = context[0];
        mode = ((const u16 *)context)[3];
        entry = context[10] != 0;
        if (channel >= 1 && channel <= 4) {
            const u32 *slot = (const u32 *)(cdc_read_states + channel * 12 + 0x7c);
            if (slot[0] == (u32)original_receive) callback = 1;
            else if (slot[0] == (u32)receive_input) callback = 2;
            else if (slot[0] == (u32)receive_wide) callback = 3;
            else if (slot[0]) callback = 4;
            context_match = slot[2] == contexts[i];
        }
        break;
    }
    if (!(flags & 0x600) && index < 0 && !debug->seen) return;
    now = sceKernelGetSystemTimeWide();
    if (now < debug->next_log || debug->emitted >= 20) return;
    debug->next_log = now + 1000000;
    ++debug->emitted;
    gobt_log("bt-session tick=%08X device=%u flags=%04X prepared=%u record=%u info=%08X name=%u caps=%04X source=%d channel=%u mode=%u entry=%u rx=%u rx-context=%u ready=%u wide=%u generation=%u accepted=%u init=%u attempts=%u good=%u bad=%u active=%d queue=%u dropped=%u\n",
             (unsigned)now, device, flags, (prepared_devices & (1u << device)) != 0,
             debug->seen, (unsigned)debug->info_result, debug->name_matches, debug->caps,
             index, channel, mode, entry, callback, context_match,
             index >= 0 ? reads_ready[index] : 0, index >= 0 ? discovery_bypassed[index] : 0,
             index >= 0 ? generations[index] : 0, source ? source->accepted : 0,
             source ? source->initialized : 0, source ? source->attempts : 0,
             source ? source->good : 0, source ? source->bad : 0,
             active_source, queued, dropped);
}
#endif

static __attribute__((noipa)) void poll_links(void)
{
    unsigned device, current = 0, lost, epoch = power_generation;
    uint64_t now = sceKernelGetSystemTimeWide();
    int interrupts;
    if (suspended || quiescing) return;
    for (device = 1; device <= 8; ++device) {
        unsigned flags = connection_flags(device);
#if PSPGOBT_SESSION_TRACE
        static uint64_t next_trace[9];
        if ((flags || ((linked_devices | disconnect_requests | closing_devices) & (1u << device)) ||
             gobt_input_bridge_known(device)) && now >= next_trace[device]) {
            unsigned i, status = 0, generation = 0;
            for (i = 0; i < 2; ++i) {
                if (((const u16 *)contexts[i])[2] != device) continue;
                generation = generations[i];
                status = reads_ready[i] | (wide_active[i] << 1) | (discovery_bypassed[i] << 2) |
                         (sources[i].accepted << 3) | (sources[i].initialized << 4);
                unsigned channel = ((const u32 *)contexts[i])[0], callback = 0, matches = 0;
                if (channel >= 1 && channel <= 4) {
                    const u32 *slot = (const u32 *)(cdc_read_states + channel * 12 + 0x7c);
                    callback = !slot[0] ? 0 : slot[0] == (u32)original_receive ? 1 :
                               slot[0] == (u32)receive_input ? 2 : slot[0] == (u32)receive_wide ? 3 : 4;
                    matches = (slot[2] == contexts[i]) | ((slot[1] == wide_native[i]) << 1) |
                              ((slot[1] == (u32)receive_buffers[i]) << 2);
                }
                gobt_session_trace(GOBT_TRACE_CONTEXT, device,
                                   channel | (((const u16 *)contexts[i])[3] << 8),
                                   callback | (matches << 8), generations[i]);
            }
            gobt_session_trace(GOBT_TRACE_LINK, device, flags, generation, status);
            next_trace[device] = now + 1000000;
        }
#endif
#if PSPGOBT_DEBUG
        debug_link_snapshot(device, flags);
#endif
        if ((flags & 2) && (flags & 0x600) && !session_closing(device)) current |= 1u << device;
    }
    interrupts = sceKernelCpuSuspendIntr();
    if (suspended || epoch != power_generation) {
        sceKernelCpuResumeIntr(interrupts);
        return;
    }
    lost = linked_devices & ~current;
    for (device = 1; device <= 8; ++device)
        if (lost & (1u << device)) {
            setup_retry_after[device] = 0;
            stream_recoveries[device] = 0;
        }
    recover_devices &= ~lost;
    serviced_devices &= current;
    pending_devices = (pending_devices & ~lost) | (current & ~serviced_devices);
    checked_devices &= ~lost;
    prepared_devices &= ~lost;
    linked_devices = current;
    for (device = 0; device < 2; ++device) {
        unsigned slot = ((const u16 *)contexts[device])[2];
        if (slot >= 1 && slot <= 8 && (lost & (1u << slot))) {
            ++generations[device];
            reads_ready[device] = 0;
            stream_started[device] = last_report[device] = 0;
            if (active_source == (int)device) {
                active_source = -1;
                pspgobtDisconnect();
            }
        } else if (slot >= 1 && slot <= 8 && (current & prepared_devices & (1u << slot)) &&
                   discovery_bypassed[device] && sources[device].accepted &&
                   stream_started[device] && stream_recoveries[slot] < 3 &&
                   now >= setup_retry_after[slot]) {
            uint64_t previous = last_report[device] ? last_report[device] : stream_started[device];
            /* An installed CDC callback is not proof of received reports.
             * Retry local input setup after two silent seconds, at most
             * three times until fresh valid input or a new link resets it. */
            if (now >= previous && now - previous >= 2000000) {
                recover_devices |= 1u << slot;
                serviced_devices &= ~(1u << slot);
                pending_devices |= 1u << slot;
            }
        }
    }
    sceKernelCpuResumeIntr(interrupts);
}

static __attribute__((noipa)) void consume(const ReceivedPacket *packet)
{
    InputSource *source = &sources[packet->source];
    unsigned generation;
    int interrupts = sceKernelCpuSuspendIntr();
    generation = generations[packet->source];
    sceKernelCpuResumeIntr(interrupts);
    if (packet->generation != generation || suspended || quiescing || session_closing(packet->device)) return;
    if (source->generation != generation) {
        memset(source, 0, sizeof(*source));
        stream_started[packet->source] = last_report[packet->source] = 0;
        source->generation = generation;
        if (active_source == (int)packet->source) {
            active_source = -1;
            pspgobtDisconnect();
        }
    }
    if (packet->event == 2) {
        gobt_log("bt-input read closed/failed source=%u generation=%u device=%u result=%08X\n",
                 packet->source, generation, packet->device, (unsigned)packet->status);
        return;
    }
    if (sceKernelGetSystemTimeWide() - packet->received >= GOBT_INPUT_TIMEOUT_US) return;
    if (!source->checked) {
        source->accepted = packet->device >= 1 && packet->device <= 8 && identify(packet->device);
        source->checked = 1;
        gobt_log("bt-input source=%u generation=%u device=%u selected=%u\n",
                 packet->source, generation, packet->device, source->accepted);
    }
    if (!source->accepted) return;
    if (packet->event == 1) {
        gobt_log("bt-input read queued source=%u generation=%u device=%u capacity=%u\n",
                 packet->source, generation, packet->device,
                 discovery_bypassed[packet->source] ? RECEIVE_CAPACITY : 64);
        initialize(packet->source, packet->device, source);
        return;
    }
    initialize(packet->source, packet->device, source);
    if (source->previous && packet->received - source->previous >= GOBT_INPUT_TIMEOUT_US)
        source->assembly.used = 0;
    source->previous = packet->received;
    if (source->assembly.used && packet->received - source->assembly_started >= GOBT_INPUT_TIMEOUT_US)
        source->assembly.used = 0;
    if (packet->event != 3 && packet->size <= RECEIVE_CAPACITY) {
        DsState state;
        if (!source->assembly.used || packet->bytes[0] == 0xa1)
            source->assembly_started = packet->received;
        int result = gobt_decode_hidp(&source->assembly, packet->bytes, packet->size, &state);
        if (result == 1) {
            if (!device_models[packet->device]) {
                unsigned report_id = packet->bytes[0] == 0xa1 && packet->size >= 2 ?
                                     packet->bytes[1] : source->assembly.report[0];
                if (report_id == 0x11) device_models[packet->device] = GOBT_MODEL_DUALSHOCK4;
                else if (report_id == 0x31) device_models[packet->device] = GOBT_MODEL_DUALSENSE;
            }
            if (packet->generation != generations[packet->source] || suspended ||
                sceKernelGetSystemTimeWide() - packet->received >= GOBT_INPUT_TIMEOUT_US) return;
            interrupts = sceKernelCpuSuspendIntr();
            if (!suspended && packet->generation == generations[packet->source]) {
                last_report[packet->source] = packet->received;
                if (packet->device >= 1 && packet->device <= 8)
                    stream_recoveries[packet->device] = 0;
            }
            sceKernelCpuResumeIntr(interrupts);
            if (!source->good) gobt_session_trace(GOBT_TRACE_INPUT, packet->device, generation, packet->source, source->initialized);
            if (active_source < 0) {
                int start = gobt_sink_start(0);
                gobt_log("bt-input native sink start result=%d source=%u\n", start, packet->source);
                if (start < 0) return;
                active_source = (int)packet->source;
            }
            if (active_source != (int)packet->source) return;
            /* Identity lookup may have blocked while the link changed. Check
             * both generation and age again before publishing decoded input. */
            interrupts = sceKernelCpuSuspendIntr();
            if (!suspended && packet->generation == generations[packet->source] &&
                sceKernelGetSystemTimeWide() - packet->received < GOBT_INPUT_TIMEOUT_US)
                result = gobt_sink_submit_state(&state);
            else result = -1;
            sceKernelCpuResumeIntr(interrupts);
            if (result >= 0 && ++source->good <= 4)
                gobt_log("bt-input forwarded source=%u reports=%u\n", packet->source, source->good);
            return;
        }
        if (result == 0) return;
    }
    if (++source->bad <= 8)
        gobt_log("bt-input unhandled source=%u bytes=%u header=%02X report=%02X\n",
                 packet->source, packet->size,
                 packet->event == 0 && packet->size <= RECEIVE_CAPACITY ? packet->bytes[0] : 0,
                 packet->event == 0 && packet->size >= 2 && packet->size <= RECEIVE_CAPACITY ? packet->bytes[1] : 0);
}

/* Caller holds interrupts and has waited for our callbacks. The manager can
 * outlive its CDC transport: restore these sites before checking/rebinding
 * that same manager image, otherwise its own guards see our old patches. */
static void restore_provider_hooks(const SceModule *provider)
{
    unsigned i;
    if (!installed || !provider || provider->modid != provider_uid ||
        provider->text_addr != hook_base ||
        !kernel_range(hook_base, GOBT_HID_CLOSE_POINTER_OFFSET + 4)) return;
    for (i = 0; i < sizeof(hook_sites) / sizeof(hook_sites[0]); ++i)
        if (_lw(hook_base + hook_sites[i]) == hook_replacement[i])
            _sw(hook_original[i], hook_base + hook_sites[i]);
    if (_lw(hook_base + GOBT_HID_CLOSE_POINTER_OFFSET) == (u32)native_close_hid)
        _sw(close_original, hook_base + GOBT_HID_CLOSE_POINTER_OFFSET);
    if (_lw(hook_base + GOBT_CONNECT_POLL_CALL_OFFSET) == connect_poll_replacement)
        _sw(connect_poll_original, hook_base + GOBT_CONNECT_POLL_CALL_OFFSET);
}

static int retire_hooks(void)
{
    SceModule *provider = sceKernelFindModuleByName("sceBSMan");
    SceModule *cdc = sceKernelFindModuleByName("sceUsbBSMCDC_Driver");
    unsigned channel;
    int interrupts;
    if (callback_depth) return 0;
    if (installed && cdc && cdc->modid == cdc_provider_uid &&
        cdc->text_addr + 0x23a0 == cdc_read_states) {
        for (channel = 1; channel <= 4; ++channel) {
            const u32 *slot = (const u32 *)(cdc_read_states + channel * 12 + 0x7c);
            if (slot[0] == (u32)receive_input || slot[0] == (u32)receive_wide ||
                slot[1] == (u32)receive_buffers[0] || slot[1] == (u32)receive_buffers[1]) return 0;
        }
    }
    interrupts = sceKernelCpuSuspendIntr();
    if (callback_depth) { sceKernelCpuResumeIntr(interrupts); return 0; }
    restore_provider_hooks(provider);
    installed = 0;
    shutdown_done = 1;
    sceKernelCpuResumeIntr(interrupts);
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    return 1;
}

static __attribute__((noipa)) int finish_shutdown(void)
{
    if (callback_depth) return 0;
    SceModule *provider = sceKernelFindModuleByName("sceBSMan");
    SceModule *cdc = sceKernelFindModuleByName("sceUsbBSMCDC_Driver");
    unsigned i;
    if (installed && provider && provider->modid == provider_uid &&
        cdc && cdc->modid == cdc_provider_uid) {
        finish_closures();
        for (i = 0; i < 2; ++i) {
            unsigned device;
            if (!contexts[i] || !discovery_bypassed[i] || (shutdown_attempted & (1u << i))) continue;
            device = ((const u16 *)contexts[i])[2];
            if (device < 1 || device > 8) continue;
            shutdown_attempted |= 1u << i;
            reset_input_context(i, device, 1);
        }
    }
    shutdown_cleanup_done = 1;
    int retired = retire_hooks();
#if PSPGOBT_SESSION_TRACE
    static unsigned last_retire_state = ~0u;
    unsigned state = retired | (wide_active[0] << 1) | (wide_active[1] << 2);
    if (state != last_retire_state) {
        gobt_session_trace(GOBT_TRACE_EXIT, 0, 2, retired, state);
        last_retire_state = state;
    }
#endif
    return retired;
}

void gobt_shutdown_input_bridge(int wait)
{
    unsigned timeout = 250000;
    gobt_session_trace(GOBT_TRACE_EXIT, 0, wait, callback_depth, shutdown_cleanup_done);
    if (shutdown_done && worker_id < 0) return;
    int interrupts = sceKernelCpuSuspendIntr();
    if (!quiescing) {
        quiescing = 1;
        ++power_generation;
        ++generations[0]; ++generations[1];
        pending_devices = checked_devices = serviced_devices = 0;
        head = tail = queued = 0;
        reads_ready[0] = reads_ready[1] = 0;
        resetting[0] = resetting[1] = 1;
        active_source = -1;
        pspgobtDisconnect();
    }
    sceKernelCpuResumeIntr(interrupts);
    /* The worker owns provider calls. Do not terminate it while it holds
     * firmware locks, or race its setup with cleanup on another thread. */
    wake_worker();
    if (wait && worker_id >= 0) {
        if (sceKernelWaitThreadEnd(worker_id, &timeout) >= 0) {
            sceKernelDeleteThread(worker_id);
            worker_id = -1;
            worker_started = 0;
            if (input_sema >= 0) sceKernelDeleteSema(input_sema);
            input_sema = -1;
        }
    }
    /* The later phase performs only guarded RAM retirement, never calls
     * back into providers that may already have run their reboot cleanup. */
    if (worker_id < 0 || (!wait && shutdown_cleanup_done)) retire_hooks();
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        /* Initial binding can start this thread before publishing provider
         * IDs and installed=1. Do not discard its half-published contexts. */
        if (binding) { sceKernelDelayThread(5000); continue; }
        if (quiescing) {
            if (finish_shutdown()) return 0;
            sceKernelDelayThread(5000);
            continue;
        }
        SceModule *provider = sceKernelFindModuleByName("sceBSMan");
        SceModule *cdc = sceKernelFindModuleByName("sceUsbBSMCDC_Driver");
        ReceivedPacket packet;
        unsigned devices, device, checked;
        int available, interrupts;
        int manager_changed = !provider || provider->modid != provider_uid || provider->text_addr != hook_base;
        int transport_changed = !cdc || cdc->modid != cdc_provider_uid || cdc->text_addr + 0x23a0 != cdc_read_states;
        if (!installed || manager_changed || transport_changed) {
            if (callback_depth) { sceKernelDelayThread(5000); continue; }
            if (installed) {
                interrupts = sceKernelCpuSuspendIntr();
                restore_provider_hooks(provider);
                installed = 0;
                ++power_generation;
                ++generations[0]; ++generations[1];
                pending_devices = checked_devices = linked_devices = serviced_devices = 0;
                prepared_devices = head = tail = queued = 0;
                reads_ready[0] = reads_ready[1] = 0;
                resetting[0] = resetting[1] = 0;
                /* A surviving manager still owns these contexts and may
                 * have submitted through the replacement CDC already.
                 * Keep DMA ownership until its actual native cancellation. */
                if (manager_changed) {
                    discovery_bypassed[0] = discovery_bypassed[1] = 0;
                    wide_active[0] = wide_active[1] = 0;
                    wide_native[0] = wide_native[1] = 0;
                    contexts[0] = contexts[1] = 0;
                }
                next_link_poll = 0;
                active_source = -1;
                memset(sources, 0, sizeof(sources));
                memset(setup_retry_after, 0, sizeof(setup_retry_after));
                memset(stream_started, 0, sizeof(stream_started));
                memset(last_report, 0, sizeof(last_report));
                memset(stream_recoveries, 0, sizeof(stream_recoveries));
                recover_devices = 0;
                if (!manager_changed && transport_changed) {
                    unsigned source;
                    for (source = 0; source < 2; ++source) {
                        unsigned slot = ((const u16 *)contexts[source])[2];
                        if (slot >= 1 && slot <= 8 && discovery_bypassed[source])
                            recover_devices |= 1u << slot;
                    }
                }
                disconnect_requests = closing_devices = pending_closures = 0;
                memset(session_epochs, 0, sizeof(session_epochs));
                memset(close_attempts, 0, sizeof(close_attempts));
                pspgobtDisconnect();
                sceKernelCpuResumeIntr(interrupts);
                sceKernelDcacheWritebackInvalidateAll();
                sceKernelIcacheInvalidateAll();
                gobt_session_trace(GOBT_TRACE_PROVIDER, 0, manager_changed, transport_changed, provider ? provider->modid : 0);
                gobt_log("bt-input provider unloaded/replaced; waiting to rebind\n");
            }
            if (manager_changed && contexts[0]) {
                interrupts = sceKernelCpuSuspendIntr();
                discovery_bypassed[0] = discovery_bypassed[1] = 0;
                wide_active[0] = wide_active[1] = 0;
                wide_native[0] = wide_native[1] = 0;
                contexts[0] = contexts[1] = 0;
                recover_devices = 0;
                sceKernelCpuResumeIntr(interrupts);
            }
            /* Rebind on this worker, between calls into the old provider.
             * Reuse its thread and power handler across module reloads. */
            if (provider && cdc && worker_started && !binding && !suspended) {
                int result = bind_provider(provider, bridge_firmware);
                gobt_session_trace(GOBT_TRACE_BIND, 0, result, provider->modid, cdc->modid);
            }
            sceKernelDelayThread(100000);
            continue;
        }
#if PSPGOBT_SESSION_TRACE
        static uint64_t trace_tick;
        uint64_t trace_now = sceKernelGetSystemTimeWide();
        if (trace_now >= trace_tick) {
            gobt_session_trace(GOBT_TRACE_TICK, 0, linked_devices, disconnect_requests, closing_devices);
            trace_tick = trace_now + 1000000;
        }
#endif
        finish_closures();
        if ((uint64_t)sceKernelGetSystemTimeWide() >= next_link_poll) {
            poll_links();
            next_link_poll = sceKernelGetSystemTimeWide() + 200000;
        }
        interrupts = sceKernelCpuSuspendIntr();
        devices = pending_devices;
        checked = checked_devices;
        pending_devices = 0;
        available = queued != 0;
        if (available) {
            packet = queue[head];
            head = (head + 1) % 16;
            --queued;
        }
        sceKernelCpuResumeIntr(interrupts);
        for (device = 1; device <= 8; ++device)
            if (devices & (1u << device)) connected(device, (checked & (1u << device)) != 0);
        unsigned processed = 0;
        if (available) {
            do {
                consume(&packet);
                ++processed;
                if (processed == INPUT_BATCH_LIMIT || quiescing) break;
                interrupts = sceKernelCpuSuspendIntr();
                available = queued != 0;
                if (available) {
                    packet = queue[head];
                    head = (head + 1) % 16;
                    --queued;
                }
                sceKernelCpuResumeIntr(interrupts);
            } while (available);
        }
        else {
            for (device = 0; device < 2; ++device)
                if (sources[device].generation == generations[device])
                    initialize(device, ((const u16 *)contexts[device])[2], &sources[device]);
        }
        gobt_sink_maintain();
        save_target_cache();
        /* Keep native callback threads and games runnable even under a
         * continuous report flood. Normal traffic sleeps until notified;
         * provider lookup and maintenance run once per batch, not report. */
        if (processed == INPUT_BATCH_LIMIT) sceKernelDelayThread(1000);
        else wait_input();
    }
    return 0;
}

static int bridge_power_event(int event, char *name, void *parameter, int *result)
{
    (void)name; (void)parameter; (void)result;
    if (quiescing) return 0;
    if (event == 0x400 || event == 0x4000 || event == 0x400000) {
        int interrupts = sceKernelCpuSuspendIntr();
        gobt_session_trace(GOBT_TRACE_POWER, 0, event, power_generation, closing_devices);
        suspended = event != 0x400000;
        ++power_generation;
        pending_devices = 0;
        checked_devices = linked_devices = serviced_devices = 0;
        next_link_poll = 0;
        prepared_devices = 0;
        memset(setup_retry_after, 0, sizeof(setup_retry_after));
        memset(stream_started, 0, sizeof(stream_started));
        memset(last_report, 0, sizeof(last_report));
        memset(stream_recoveries, 0, sizeof(stream_recoveries));
        recover_devices = 0;
        /* A surviving native context still owns a wide staging read. Keep
         * its discovery route until actual native setup replaces it; the
         * next register_hid_service call refreshes this per-context flag. */
        reads_ready[0] = reads_ready[1] = 0;
        ++generations[0]; ++generations[1];
        head = tail = queued = 0;
        active_source = -1;
        pspgobtDisconnect();
        sceKernelCpuResumeIntr(interrupts);
        wake_worker();
    }
    return 0;
}

static PspSysEventHandler power_handler = {
    .size = sizeof(PspSysEventHandler), .name = "PSPGoBTBridge",
    .type_mask = 0x00ffff00, .handler = bridge_power_event
};

int gobt_start_input_bridge(unsigned long firmware)
{
    SceUID thread, marker;
    int result;
    if (quiescing) return -9;
    if (worker_started) return 0;
    if (firmware != 0x06060010 && firmware != 0x06060110) return -2;
    marker = sceIoOpen("ef0:/seplugins/pspgobt/input-test", PSP_O_RDONLY, 0);
    if (marker >= 0) { sceIoClose(marker); return -7; }
    load_target_cache();
    result = sceKernelRegisterSysEventHandler(&power_handler);
    if (result < 0) return result;
    input_sema = sceKernelCreateSema("PSPGoBTInputReady", 0, 0, 1, NULL);
    if (input_sema < 0) {
        sceKernelUnregisterSysEventHandler(&power_handler);
        return input_sema;
    }
    /* Short event-driven input work precedes common game priorities. It
     * blocks when idle and yields after a bounded burst; never busy-poll. */
    thread = sceKernelCreateThread("PSPGoBTInput", worker, 0x10, 0x3000, 0, NULL);
    if (thread < 0) {
        sceKernelDeleteSema(input_sema);
        input_sema = -1;
        sceKernelUnregisterSysEventHandler(&power_handler);
        return thread;
    }
    bridge_firmware = firmware;
    worker_id = thread;
    worker_started = 1;
    result = sceKernelStartThread(thread, 0, NULL);
    if (result < 0) {
        worker_started = 0;
        worker_id = -1;
        sceKernelDeleteThread(thread);
        sceKernelDeleteSema(input_sema);
        input_sema = -1;
        sceKernelUnregisterSysEventHandler(&power_handler);
        return result;
    }
    return 0;
}

static int bind_provider(SceModule *module, unsigned long firmware)
{
    SceModule *cdc;
    SceUID marker;
    int interrupts, result;
    u32 targets[] = {(u32)submit_first, (u32)submit_next,
                     (u32)trace_native_identity, (u32)register_hid_service,
                     (u32)native_setup_event, (u32)native_setup_checked,
                     (u32)native_setup_flags, (u32)native_session_event, (u32)connect_poll_stub};
    const unsigned sites[] = {GOBT_RX_FIRST_CALL_OFFSET, GOBT_RX_NEXT_CALL_OFFSET,
                               GOBT_RX_IDENTITY_CALL_OFFSET, GOBT_RX_SERVICE_CALL_OFFSET,
                               GOBT_SETUP_EVENT_CALL_OFFSET, GOBT_SETUP_CHECKED_CALL_OFFSET,
                               GOBT_SETUP_FLAGS_CALL_OFFSET, GOBT_SESSION_EVENT_CALL_OFFSET};
    unsigned i;
    if (!module || strcmp(module->modname, "sceBSMan")) return -1;
    if (!gobt_report_bridge_supported((uint32_t)firmware, (const uint8_t *)module->text_addr,
                                      module->text_size, module->text_addr)) return -2;
    if (!gobt_input_setup_supported((const uint8_t *)module->text_addr,
                                    module->text_size, module->text_addr)) return -2;
    if (!gobt_cached_record_supported((const uint8_t *)module->text_addr,
                                      module->text_size, module->text_addr)) return -2;
    if (!gobt_input_discovery_supported((const uint8_t *)module->text_addr,
                                        module->text_size, module->text_addr)) return -2;
    if (!gobt_reconnect_supported((const uint8_t *)module->text_addr,
                                  module->text_size, module->text_addr)) return -2;
    if (!gobt_hid_reset_supported((const uint8_t *)module->text_addr,
                                  module->text_size, module->text_addr)) return -2;
    if (!gobt_hid_close_supported((const uint8_t *)module->text_addr,
                                  module->text_size, module->text_addr)) return -2;
    if (!gobt_session_event_supported((const uint8_t *)module->text_addr,
                                      module->text_size, module->text_addr)) return -2;
    if (!gobt_connect_epoch_supported((const uint8_t *)module->text_addr,
                                      module->text_size, module->text_addr)) return -2;
    marker = sceIoOpen("ef0:/seplugins/pspgobt/input-test", PSP_O_RDONLY, 0);
    if (marker >= 0) { sceIoClose(marker); return -7; }
    cdc = sceKernelFindModuleByName("sceUsbBSMCDC_Driver");
    original_submit = (ReadSubmit)sctrlHENFindFunction("sceUsbBSMCDC_Driver", "sceUsbBSMCDC_driver", 0xa5e108bb);
    get_info = (void *)sctrlHENFindFunction("sceBSMan", "sceBSMan", 0xcbd232d1);
    write_hid = (void *)sctrlHENFindFunction("sceUsbBSMCDC_Driver", "sceUsbBSMCDC_driver", 0xf8c6afbd);
    if (!cdc || !original_submit || !write_hid || !get_info) return -3;
    if (!gobt_cdc_read_state_supported((const uint8_t *)cdc->text_addr, cdc->text_size, cdc->text_addr)) return -3;
    cdc_read_states = cdc->text_addr + 0x23a0;
    {
        const u32 expected[] = {0x27bdffe0, 0xafb10004, 0x3c02802f, 0x00808821};
        uintptr_t entry = (uintptr_t)original_submit;
        if (cdc->text_size < 48 || entry < cdc->text_addr ||
            entry - cdc->text_addr > cdc->text_size - 48 ||
            memcmp((const void *)entry, expected, sizeof(expected)) ||
            ((const u32 *)entry)[11] != 0x01009821) return -3;
        if (_lw(module->text_addr + GOBT_RX_SUBMIT_OFFSET) !=
            (0x08000000u | (((u32)original_submit >> 2) & 0x03ffffffu)) ||
            _lw(module->text_addr + GOBT_RX_SUBMIT_OFFSET + 4) != 0) return -3;
        if ((uintptr_t)get_info < module->text_addr ||
            (uintptr_t)get_info - module->text_addr > module->text_size - 16) return -3;
        if (!gobt_bsman_api_supported(0xcbd232d1, (const uint8_t *)get_info, 16)) return -3;
        {
            const u32 write_expected[] = {0x27bdffe0, 0xafb20008, 0x3c02802f, 0x00809021};
            uintptr_t writer = (uintptr_t)write_hid;
            if (writer < cdc->text_addr || writer - cdc->text_addr > cdc->text_size - sizeof(write_expected) ||
                memcmp((const void *)writer, write_expected, sizeof(write_expected))) return -3;
        }
    }
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        if (((module->text_addr + sites[i] + 4) ^ targets[i]) & 0xf0000000u) return -4;
    if (((module->text_addr + GOBT_CONNECT_POLL_CALL_OFFSET + 4) ^ targets[8]) & 0xf0000000u) return -4;
    if ((((u32)connect_poll_stub + 12) ^ (u32)native_connect_poll) & 0xf0000000u) return -4;
    original_receive = (ReadCallback)(module->text_addr + GOBT_RX_CALLBACK_OFFSET);
    start_hid = (void *)(module->text_addr + 0xa128);
    stop_hid = (void *)(module->text_addr + 0xa2dc);
    disconnect_hid = (void *)(module->text_addr + 0x2b10);
    session_event = (void *)(module->text_addr + 0x2044);
    finish_disconnect = (void *)(module->text_addr + 0x255c);
    native_poll_flags = (void *)(module->text_addr + 0xcab8);
    native_clear_flags = (void *)(module->text_addr + 0xcb20);
    native_identity = (void *)(module->text_addr + 0x58d8);
    native_register = (void *)(module->text_addr + 0xcbf8);
    connection_flags = (void *)(module->text_addr + 0x7e0);
    paired_records = module->text_addr + 0xdd00;
    record_command = (void *)(module->text_addr + 0x3720);
    get_info = cached_info;
    contexts[0] = module->text_addr + GOBT_RX_CONTEXT_OFFSET;
    contexts[1] = contexts[0] + 48;
    result = gobt_start_input_bridge(firmware);
    if (result < 0) return result;
    provider_uid = module->modid;
    cdc_provider_uid = cdc->modid;
    hook_base = module->text_addr;
    connect_poll_stub[0] = 0x02004021; /* move t0,s0: fifth EABI argument, reserved entry */
    connect_poll_stub[1] = 0x02604821; /* move t1,s3: sixth EABI argument, device */
    connect_poll_stub[2] = 0x08000000u | (((u32)native_connect_poll >> 2) & 0x03ffffffu);
    connect_poll_stub[3] = 0;
    /* Publish executable adapter data before a live call can target it. */
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    interrupts = sceKernelCpuSuspendIntr();
    connect_poll_original = _lw(module->text_addr + GOBT_CONNECT_POLL_CALL_OFFSET);
    connect_poll_replacement = 0x0c000000u | ((targets[8] >> 2) & 0x03ffffffu);
    _sw(connect_poll_replacement, module->text_addr + GOBT_CONNECT_POLL_CALL_OFFSET);
    close_original = _lw(module->text_addr + GOBT_HID_CLOSE_POINTER_OFFSET);
    _sw((u32)native_close_hid, module->text_addr + GOBT_HID_CLOSE_POINTER_OFFSET);
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        hook_original[i] = _lw(module->text_addr + sites[i]);
        hook_replacement[i] = 0x0c000000u | ((targets[i] >> 2) & 0x03ffffffu);
        _sw(hook_replacement[i], module->text_addr + sites[i]);
    }
    sceKernelCpuResumeIntr(interrupts);
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    installed = 1;
    worker_started = 1;
    gobt_log("bt-input bridge installed first=%08lX next=%08lX; native sink starts after identified reports\n",
             module->text_addr + sites[0], module->text_addr + sites[1]);
    return 0;
}

int gobt_install_input_bridge(SceModule *module, unsigned long firmware)
{
    int result;
    if (quiescing) return -9;
    if (!module || strcmp(module->modname, "sceBSMan")) return -1;
    /* Module-start callbacks must not race a live worker's provider calls.
     * A resident worker detects the changed UID and performs its own rebind. */
    if (worker_started) return installed && module->modid == provider_uid ? 0 : 1;
    bridge_firmware = firmware;
    binding = 1;
    result = bind_provider(module, firmware);
    gobt_session_trace(GOBT_TRACE_BIND, 0, result, module->modid, cdc_provider_uid);
    binding = 0;
    return result;
}

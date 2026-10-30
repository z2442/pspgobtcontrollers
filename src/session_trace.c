#include "session_trace.h"
#if PSPGOBT_SESSION_TRACE
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman_kernel.h>

/* Callback producers only copy numbers to RAM. A separate thread writes the
 * file, even if a Bluetooth call on the input/UI worker stops returning.
 * No packet contents, addresses, names, keys or firmware dumps are recorded. */
#define TRACE_PATH "ef0:/seplugins/pspgobt/session.log"
#define TRACE_CAPACITY 128u
#define TRACE_LIMIT 1024u
#define TRACE_FILE_LIMIT (256 * 1024)
typedef struct { unsigned words[7]; } TraceRecord;
static TraceRecord trace_ring[TRACE_CAPACITY];
static unsigned trace_head, trace_tail, trace_count, trace_sequence, trace_written;
static int trace_enabled, trace_stopping;
static SceUID trace_thread = -1;

void gobt_session_trace(unsigned kind, unsigned device, unsigned a, unsigned b, unsigned c)
{
    unsigned k1 = pspSdkSetK1(0);
    int interrupts = sceKernelCpuSuspendIntr();
    if (trace_enabled && !trace_stopping && trace_sequence < TRACE_LIMIT) {
        unsigned sequence = ++trace_sequence;
        if (trace_count < TRACE_CAPACITY) {
            TraceRecord *record = &trace_ring[trace_tail];
            record->words[0] = (unsigned)sceKernelGetSystemTimeWide();
            record->words[1] = sequence;
            record->words[2] = kind;
            record->words[3] = device;
            record->words[4] = a; record->words[5] = b; record->words[6] = c;
            trace_tail = (trace_tail + 1) % TRACE_CAPACITY;
            ++trace_count;
        }
    }
    sceKernelCpuResumeIntr(interrupts);
    pspSdkSetK1(k1);
}

static int trace_writer(SceSize args, void *argp)
{
    SceUID fd;
    (void)args; (void)argp;
    fd = sceIoOpen(TRACE_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
    if (fd < 0) { trace_enabled = 0; return 0; }
    SceOff length = sceIoLseek(fd, 0, PSP_SEEK_END);
    /* Leave room for this entire instance; retain several XMB/game handoffs.
     * An oversized old trace is replaced, never allowed to grow unbounded. */
    if (length < 0 || length > TRACE_FILE_LIMIT - TRACE_LIMIT * 63) {
        sceIoClose(fd);
        fd = sceIoOpen(TRACE_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
        if (fd < 0) { trace_enabled = 0; return 0; }
    }
    for (;;) {
        TraceRecord record;
        char line[63];
        unsigned i, j;
        int interrupts = sceKernelCpuSuspendIntr();
        if (!trace_count) {
            int stop = trace_stopping || trace_sequence >= TRACE_LIMIT;
            sceKernelCpuResumeIntr(interrupts);
            if (stop) break;
            sceKernelDelayThread(50000);
            continue;
        }
        record = trace_ring[trace_head];
        trace_head = (trace_head + 1) % TRACE_CAPACITY;
        --trace_count;
        sceKernelCpuResumeIntr(interrupts);
        for (i = 0; i < 7; ++i) {
            for (j = 0; j < 8; ++j)
                line[i * 9 + j] = "0123456789ABCDEF"[(record.words[i] >> (28 - 4 * j)) & 15];
            line[i * 9 + 8] = i == 6 ? '\n' : ' ';
        }
        if (sceIoWrite(fd, line, sizeof(line)) != sizeof(line)) break;
        ++trace_written;
    }
    trace_enabled = 0;
    sceIoClose(fd);
    return 0;
}

void gobt_session_trace_start(unsigned firmware)
{
    if (trace_thread >= 0) return;
    trace_thread = sceKernelCreateThread("PSPGoBTSessionTrace", trace_writer, 0x30, 0x1800, 0, NULL);
    if (trace_thread < 0) return;
    trace_enabled = 1;
    gobt_session_trace(GOBT_TRACE_BOOT, 0, 28, firmware, 0);
    if (sceKernelStartThread(trace_thread, 0, NULL) < 0) {
        trace_enabled = 0;
        sceKernelDeleteThread(trace_thread);
        trace_thread = -1;
    }
}

void gobt_session_trace_stop(void)
{
    SceUInt timeout = 100000;
    gobt_session_trace(GOBT_TRACE_STOP, 0, trace_written, 0, 0);
    int interrupts = sceKernelCpuSuspendIntr();
    trace_stopping = 1;
    sceKernelCpuResumeIntr(interrupts);
    if (trace_thread >= 0 && sceKernelWaitThreadEnd(trace_thread, &timeout) >= 0) {
        sceKernelDeleteThread(trace_thread);
        trace_thread = -1;
    }
}
#endif

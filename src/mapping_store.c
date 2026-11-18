#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman_kernel.h>
#include <psputilsforkernel.h>
#include <string.h>
#include "bt_input_bridge.h"
#include "mapping_ui.h"
#include "mapping_patch.h"
#include "session_trace.h"

#define PATH "ef0:/seplugins/pspgobt/mappings.cfg"
static GoBtMapBook mappings;
static GoBtMap active_map;
static uint8_t active_address[6];
static int selected;
static int primary_valid;
static volatile int closing;
static SceUID lock = -1;

static int read_book(const char *path)
{
    uint8_t bytes[GOBT_MAP_FILE_BYTES + 1];
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    int count;
    if (fd < 0) return -1;
    count = sceIoRead(fd, bytes, sizeof(bytes));
    sceIoClose(fd);
    return count < 0 ? -1 : gobt_map_decode(&mappings, bytes, count);
}

int gobt_mapping_start(void)
{
    if (lock >= 0) return 0;
    lock = sceKernelCreateSema("PSPGoBTMapping", 0, 1, 1, NULL);
    if (lock < 0) return lock;
    primary_valid = read_book(PATH) == 0;
    if (!primary_valid) read_book(PATH ".bak");
    return 0;
}

void gobt_mapping_shutdown(void)
{
    closing = 1;
}

int gobt_mapping_closing(void)
{
    return closing;
}

void gobt_mapping_select(const uint8_t address[6])
{
    int index, interrupts = sceKernelCpuSuspendIntr();
    gobt_map_default(&active_map);
    selected = address != NULL;
    if (address) {
        memcpy(active_address, address, 6);
        index = gobt_map_find(&mappings, address);
        if (index >= 0) active_map = mappings.record[index].map;
    }
    sceKernelCpuResumeIntr(interrupts);
}

void gobt_mapping_apply(const DsState *source, DsState *destination)
{
    /* Called with the sink's interrupts disabled; no storage or waits here. */
    if (selected) gobt_map_apply(&active_map, source, destination);
    else *destination = *source;
}

/* Save runs on the caller's kernel stack beneath IoFileMgr. Keep large
 * scratch out of that frame; both buffers are only used under the lock. */
static uint8_t encoded[GOBT_MAP_FILE_BYTES];
static GoBtMapBook updated;

static int write_book(const GoBtMapBook *book)
{
    uint8_t *bytes = encoded;
    int result, closed, renamed;
    SceUID fd;
    if (gobt_map_encode(book, bytes) < 0) return -1;
    fd = sceIoOpen(PATH ".tmp", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) return fd;
    result = sceIoWrite(fd, bytes, GOBT_MAP_FILE_BYTES);
    closed = sceIoClose(fd);
    if (result != GOBT_MAP_FILE_BYTES || closed < 0) {
        sceIoRemove(PATH ".tmp");
        return -1;
    }
    renamed = -1;
    if (primary_valid) {
        sceIoRemove(PATH ".bak");
        renamed = sceIoRename(PATH, PATH ".bak");
        if (renamed < 0) { sceIoRemove(PATH ".tmp"); return renamed; }
    } else {
        /* Preserve a valid recovery file while replacing a damaged primary. */
        sceIoRemove(PATH);
    }
    result = sceIoRename(PATH ".tmp", PATH);
    if (result < 0 && renamed >= 0)
        primary_valid = sceIoRename(PATH ".bak", PATH) >= 0;
    if (result >= 0) primary_valid = 1;
    return result;
}

/* Retain all currently paired controllers when the eight-record book fills. */
static void prune_book(GoBtMapBook *book)
{
    uint8_t addresses[8][6];
    unsigned i, j, kept = 0, count = 0;
    for (i = 1; i <= 8; ++i)
        if (gobt_input_bridge_paired_address(i, addresses[count])) ++count;
    for (i = 0; i < book->count; ++i) {
        for (j = 0; j < count; ++j)
            if (!memcmp(book->record[i].address, addresses[j], 6)) break;
        if (j < count) book->record[kept++] = book->record[i];
    }
    book->count = kept;
}

static int user_range(uintptr_t pointer, unsigned size)
{
    return !(pointer & 3) && pointer >= 0x08800000u && pointer <= 0x0c000000u - size;
}

static int install_patch(const GoBtMappingPatch *user)
{
    GoBtMappingPatch patch;
    uint32_t *append, *generic;
    int interrupts;
    unsigned k1;
    if (closing || !user_range((uintptr_t)user, sizeof(patch))) return -1;
    k1 = pspSdkSetK1(0);
    memcpy(&patch, user, sizeof(patch));
    if (patch.abi != GOBT_MAPPING_UI_ABI || !user_range(patch.append, GOBT_MAPPING_STUB_WORDS * 4) ||
        !user_range(patch.bypass, GOBT_MAPPING_BYPASS_WORDS * 4) ||
        ((patch.append ^ patch.base) & 0xf0000000u) || ((patch.bypass ^ patch.base) & 0xf0000000u) ||
        !gobt_mapping_supported(sceKernelDevkitVersion(), (const uint8_t *)patch.base, patch.size, patch.base)) {
        pspSdkSetK1(k1);
        return -1;
    }
    append = (uint32_t *)(patch.base + GOBT_MAPPING_APPEND);
    generic = (uint32_t *)(patch.base + GOBT_MAPPING_GENERIC_JUMP);
    /* Publish both sites together. The generic option path jumps to
     * APPEND+4 directly, so it must be redirected in the same step that
     * replaces that instruction with the hook's delay slot. */
    interrupts = sceKernelCpuSuspendIntr();
    generic[0] = 0x08000000u | ((patch.bypass >> 2) & 0x03ffffffu);
    append[0] = 0x08000000u | ((patch.append >> 2) & 0x03ffffffu);
    append[1] = 0;
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    sceKernelCpuResumeIntr(interrupts);
    pspSdkSetK1(k1);
    return 0;
}

int pspgobtMappingService(unsigned command, void *user)
{
    GoBtMappingRequest request;
    uint8_t address[6];
    uintptr_t pointer = (uintptr_t)user;
    unsigned k1;
    int result = -1, index, interrupts;
    if (command == GOBT_MAPPING_FLUSH) {
        sceKernelDcacheWritebackInvalidateAll();
        sceKernelIcacheInvalidateAll();
        return 0;
    }
    if (command == GOBT_MAPPING_TRACE) {
        uint32_t words[4];
        if (pointer < 0x08800000u || pointer > 0x0c000000u - sizeof(words) || (pointer & 3)) return -1;
        k1 = pspSdkSetK1(0);
        memcpy(words, user, sizeof(words));
        pspSdkSetK1(k1);
        if (words[0] >= 5 && words[0] <= 15)
            gobt_session_trace(GOBT_TRACE_MAPPING, words[0], words[1], words[2], words[3]);
        return 0;
    }
    if (command == GOBT_MAPPING_PATCH) {
        result = install_patch(user);
        gobt_session_trace(GOBT_TRACE_MAPPING, 3, (unsigned)result, 0, 0);
        return result;
    }
    if (pointer < 0x08800000u || pointer > 0x0c000000u - sizeof(request) || (pointer & 3)) return -1;
    k1 = pspSdkSetK1(0);
    memcpy(&request, user, sizeof(request));
    if (closing || lock < 0 || request.abi != GOBT_MAPPING_ABI || request.device < 1 || request.device > 8 ||
        (command != GOBT_MAPPING_GET && command != GOBT_MAPPING_SAVE)) goto done;
    if (command == GOBT_MAPPING_GET) {
        /* Menu construction is read-only and must never wait on storage,
         * another editor, or a Bluetooth manager command. Writers publish
         * their completed book with interrupts disabled as well. */
        interrupts = sceKernelCpuSuspendIntr();
        if (!gobt_input_bridge_mapping_address(request.device, address)) {
            sceKernelCpuResumeIntr(interrupts);
            gobt_session_trace(GOBT_TRACE_MAPPING, 4, request.device, 0, 0);
            goto done;
        }
        index = gobt_map_find(&mappings, address);
        gobt_map_default(&request.map);
        if (index >= 0) request.map = mappings.record[index].map;
        sceKernelCpuResumeIntr(interrupts);
        memcpy(request.address, address, 6);
        memset(request.reserved, 0, sizeof(request.reserved));
        memcpy(user, &request, sizeof(request));
        result = 0;
        gobt_session_trace(GOBT_TRACE_MAPPING, 4, request.device, 0, 1);
    } else {
        if (sceKernelWaitSema(lock, 1, NULL) < 0) goto done;
        if (!gobt_input_bridge_mapping_address(request.device, address)) goto unlock;
        if (memcmp(address, request.address, 6) || !gobt_map_valid(&request.map)) goto unlock;
        updated = mappings;
        if (updated.count == GOBT_MAP_RECORDS && gobt_map_find(&updated, address) < 0) prune_book(&updated);
        if (gobt_map_put(&updated, address, &request.map) < 0) goto unlock;
        if (!gobt_input_bridge_paired_address(request.device, address) || memcmp(address, request.address, 6)) goto unlock;
        result = write_book(&updated);
        if (result >= 0) {
            interrupts = sceKernelCpuSuspendIntr();
            mappings = updated;
            if (selected && !memcmp(active_address, request.address, 6)) active_map = request.map;
            sceKernelCpuResumeIntr(interrupts);
        }
        sceKernelSignalSema(lock, 1);
        goto done;
    }
    goto done;
unlock:
    sceKernelSignalSema(lock, 1);
done:
    pspSdkSetK1(k1);
    return result;
}

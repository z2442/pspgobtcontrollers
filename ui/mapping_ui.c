#include <pspkernel.h>
#include <stdint.h>
#include <string.h>
#include "mapping_ui.h"
#include "mapping_patch.h"

PSP_MODULE_INFO("PSPGoBT_Mapping", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

/* Native Bluetooth/PAF objects stay entirely in user mode. No rendering or
 * controller polling thread: the XMB dispatches these ordinary menu actions.
 *
 * Selecting a row makes the native dispatcher start closing the page before
 * it calls the row's action, and that close finishes asynchronously. Any
 * reopen we start, during the action or at close completion, is overtaken by
 * the page's own close: hardware showed an empty page or a freeze. Actions
 * therefore only record the next screen. The user's next Triangle press opens
 * the menu natively, and the builder hook shows that screen instead. */
typedef void (*Action)(void *, void *, void *);
typedef int (*Service)(unsigned, void *);
enum { VIEW_NONE, VIEW_MAIN, VIEW_CHOICE };
#define ENTRY_LABEL "Controller Mapping (then press Triangle)"
#define HINT_LABEL "After selecting, press Triangle to continue" 
static uint32_t bt_base;
static Service service;
static void *mapping_menu, *device_widget;
static GoBtMappingRequest request;
static unsigned field, row_count;
static unsigned pending_view, pending_selection;
static const char *pending_error;
static uint16_t labels[32][96];
static uint32_t append_stub[GOBT_MAPPING_STUB_WORDS] __attribute__((aligned(16)));
static uint32_t bypass_stub[GOBT_MAPPING_BYPASS_WORDS] __attribute__((aligned(16)));

static const char *const buttons[GOBT_MAP_BUTTONS] = {
    "Select / Create / -", "L3", "R3", "Start / Options / +",
    "Up", "Right", "Down", "Left", "L2 / ZL", "R2 / ZR",
    "L1 / L", "R1 / R", "Triangle / X", "Circle / A",
    "Cross / B", "Square / Y", "PS / Home"
};
static const char *const targets[GOBT_MAP_BUTTONS] = {
    "Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left",
    "L2 (PSP L)", "R2 (PSP R)", "L1 (PSP L)", "R1 (PSP R)",
    "Triangle", "Circle", "Cross", "Square", "PS / Home"
};

static void format_label(char label[96], const char *name, const char *value)
{
    unsigned used = 0;
    while (*name && used < 92) label[used++] = *name++;
    label[used++] = ':';
    label[used++] = ' ';
    while (*value && used < 95) label[used++] = *value++;
    label[used] = 0;
}

static int user_pointer(const void *p, unsigned size)
{
    uintptr_t value = (uintptr_t)p;
    return !(value & 3) && value >= 0x08800000u && size <= 0x03800000u && value <= 0x0c000000u - size;
}

static uintptr_t method(void *object, unsigned offset)
{
    const uintptr_t *table;
    if (!user_pointer(object, 4)) return 0;
    table = *(const uintptr_t **)object;
    if (!user_pointer(table, offset + 4) ||
        !user_pointer((const void *)table[offset / 4], 4)) return 0;
    return table[offset / 4];
}

static void *global(unsigned offset)
{
    return *(void **)(bt_base + offset);
}

static unsigned selected_device(void *widget)
{
    uint32_t *manager = global(0x20d5c), *order, *profiles;
    unsigned selection, index, capacity;
    uintptr_t array;
    uintptr_t get_index = method(widget, 0x194);
    if (!get_index || !user_pointer(manager, 36)) return 0;
    selection = ((unsigned (*)(void *))get_index)(widget);
    /* Only the display order is a vector. Profiles are a nine-element new[]
     * array (reserved element zero plus eight device slots), with capacity
     * at +0x14 and the allocation cookie immediately before the array. The
     * unrelated field at +0x1c is normally zero, not an element count. */
    capacity = manager[5];
    if (manager[2] > 8 || selection >= manager[2] || !capacity || capacity > 9) return 0;
    order = (uint32_t *)manager[1];
    array = manager[6];
    if (!user_pointer(order, manager[2] * 4) || array < 0x08800004u ||
        !user_pointer((const void *)(array - 4), 4 + capacity * 44)) return 0;
    profiles = (uint32_t *)array;
    if (profiles[-1] != capacity) return 0;
    index = order[selection];
    if (!index || index >= capacity || profiles[index * 11 + 1] != 9) return 0;
    index = profiles[index * 11];
    return index >= 1 && index <= 8 ? index : 0;
}

static void add_row(void *menu, const char *text, Action action, unsigned value)
{
    uint32_t entry[5] = {0};
    const uint32_t *native = (const uint32_t *)bt_base;
    unsigned i, row = row_count++;
    if (!user_pointer(menu, 0x5c) || row >= 32) return;
    for (i = 0; text[i] && i < 95; ++i) labels[row][i] = (uint8_t)text[i];
    labels[row][i] = 0;
    /* Use Sony's own wide-string constructor, vector copy and destructor.
     * Only our new row needs a supplied string: leave the module's shared
     * resource lookup and every original item's callback untouched. Obtain
     * the empty-string sentinel from the live, relocated native Add method. */
    entry[0] = (native[0x6f60 / 4] << 16) + (int16_t)native[0x6f68 / 4];
    entry[3] = (uintptr_t)action;
    entry[4] = value;
    ((void (*)(void *, const uint16_t *))(bt_base + GOBT_MAPPING_WIDE_CTOR))(entry, labels[row]);
    ((void (*)(void *, const void *))(bt_base + GOBT_MAPPING_VECTOR_PUSH))((uint8_t *)menu + 0x4c, entry);
    ((void (*)(void *))(bt_base + GOBT_MAPPING_WIDE_DTOR))(entry);
}

/* Session-trace builds record these; other builds ignore them. */
static void note(unsigned stage, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t words[4] __attribute__((aligned(4))) = {stage, a, b, c};
    service(GOBT_MAPPING_TRACE, words);
}

static uint32_t menu_state(void *menu)
{
    const uint32_t *state = menu;
    if (!user_pointer(menu, 0x5c)) return 0xffffffffu;
    return state[GOBT_MAPPING_MENU_FLAGS / 4] | (state[GOBT_MAPPING_MENU_PAGE / 4] ? 0x10000u : 0);
}

static void clear_menu(void *menu)
{
    /* Sony's own reset of both item vectors, as its close handler uses. */
    ((void (*)(void *))(bt_base + GOBT_MAPPING_CLEAR))(menu);
    row_count = 0;
}

static void request_view(unsigned view, unsigned selection, const char *error)
{
    note(5, view, field, menu_state(mapping_menu));
    pending_view = view;
    pending_selection = selection;
    pending_error = error;
}

static void pick(void *menu, void *event, void *value)
{
    unsigned target = (uintptr_t)value;
    (void)menu; (void)event;
    if (field < GOBT_MAP_BUTTONS) request.map.button[field] = target;
    else if (field == 17) request.map.stick = target;
    else if (field == 18) request.map.flags = (request.map.flags & ~1u) | target;
    else if (field == 19) request.map.flags = (request.map.flags & ~2u) | (target << 1);
    request_view(VIEW_MAIN, field, NULL);
}

static void choose(void *menu, void *event, void *value)
{
    unsigned selection;
    (void)menu; (void)event;
    field = (uintptr_t)value;
    if (field < GOBT_MAP_BUTTONS)
        selection = request.map.button[field] == GOBT_MAP_OFF ? 17 : request.map.button[field];
    else if (field == 17) selection = request.map.stick;
    else selection = (request.map.flags >> (field - 18)) & 1;
    request_view(VIEW_CHOICE, selection, NULL);
}

static void save(void *menu, void *event, void *value)
{
    (void)menu; (void)event; (void)value;
    if (service(GOBT_MAPPING_SAVE, &request) < 0)
        request_view(VIEW_MAIN, 20, "Save failed. Check storage and paired device.");
    /* Success lets the native close finish, back at the Bluetooth list. */
}

static void defaults(void *menu, void *event, void *value)
{
    (void)menu; (void)event; (void)value;
    gobt_map_default(&request.map);
    request_view(VIEW_MAIN, 21, NULL);
}

static void cancel(void *menu, void *event, void *value)
{
    (void)menu; (void)event; (void)value;
}

static void render_main(void *menu)
{
    char label[96];
    unsigned i;
    for (i = 0; i < GOBT_MAP_BUTTONS; ++i) {
        unsigned target = request.map.button[i];
        format_label(label, buttons[i], target == GOBT_MAP_OFF ? "Disabled" : targets[target]);
        add_row(menu, label, choose, i);
    }
    format_label(label, "PSP analog", request.map.stick == 0 ? "Left stick" : request.map.stick == 1 ? "Right stick" : "Disabled");
    add_row(menu, label, choose, 17);
    format_label(label, "Analog X", request.map.flags & 1 ? "Inverted" : "Normal");
    add_row(menu, label, choose, 18);
    format_label(label, "Analog Y", request.map.flags & 2 ? "Inverted" : "Normal");
    add_row(menu, label, choose, 19);
    add_row(menu, "Save Mapping", save, 0);
    add_row(menu, "Restore Defaults", defaults, 0);
    add_row(menu, "Cancel", cancel, 0);
    add_row(menu, HINT_LABEL, NULL, 0);
    if (pending_error) add_row(menu, pending_error, NULL, 0);
}

static void render_choice(void *menu)
{
    unsigned i;
    if (field < GOBT_MAP_BUTTONS) {
        for (i = 0; i < GOBT_MAP_BUTTONS; ++i) add_row(menu, targets[i], pick, i);
        add_row(menu, "Disabled", pick, GOBT_MAP_OFF);
    } else if (field == 17) {
        add_row(menu, "Left stick", pick, 0);
        add_row(menu, "Right stick", pick, 1);
        add_row(menu, "Disabled", pick, 2);
    } else {
        add_row(menu, "Normal", pick, 0);
        add_row(menu, "Inverted", pick, 1);
    }
    add_row(menu, HINT_LABEL, NULL, 0);
}

static void open_mapping(void *menu, void *event, void *value)
{
    GoBtMappingRequest current;
    (void)event; (void)value;
    memset(&current, 0, sizeof(current));
    current.abi = GOBT_MAPPING_ABI;
    current.device = selected_device(device_widget);
    if (!current.device || current.device != request.device || service(GOBT_MAPPING_GET, &current) < 0 ||
        memcmp(current.address, request.address, 6)) {
        note(5, 0, current.device, request.device);
        return;
    }
    request = current;
    mapping_menu = menu;
    request_view(VIEW_MAIN, 0, NULL);
}


void mapping_append(void *menu, void *widget, void *parent)
{
    GoBtMappingRequest current;
    unsigned view = VIEW_NONE;
    (void)parent; /* The stub keeps passing it; native open owns the parent. */
    /* The next native open of the same controller's menu shows the screen
     * chosen by the last action. Another controller discards it. */
    if (pending_view && menu == mapping_menu) {
        if (selected_device(widget) == request.device) view = pending_view;
        else note(9, pending_view, selected_device(widget), request.device);
        pending_view = VIEW_NONE;
    }
    if (view && menu == mapping_menu) {
        /* Replace the native rows just added with the editor, before the
         * builder binds and shows the page. */
        clear_menu(menu);
        if (view == VIEW_CHOICE) render_choice(menu);
        else render_main(menu);
        if (pending_selection < row_count) ((uint32_t *)menu)[0x44 / 4] = pending_selection;
        pending_error = NULL;
        note(8, view, row_count, menu_state(menu));
        return;
    }
    pending_view = VIEW_NONE;
    memset(&current, 0, sizeof(current));
    current.abi = GOBT_MAPPING_ABI;
    current.device = selected_device(widget);
    if (!current.device || service(GOBT_MAPPING_GET, &current) < 0) return;
    request = current;
    device_widget = widget;
    row_count = 0;
    add_row(menu, ENTRY_LABEL, open_mapping, 0);
}

int module_start(SceSize size, void *data)
{
    GoBtMappingUiArgs args;
    GoBtMappingPatch patch;
    uint32_t *append;
    if (size != sizeof(args) || !data) return -1;
    memcpy(&args, data, sizeof(args));
    if (args.abi != GOBT_MAPPING_UI_ABI || !user_pointer((void *)args.gateway, 8) ||
        !gobt_mapping_supported(0x06060110, (const uint8_t *)args.base, args.size, args.base)) return -1;
    bt_base = args.base;
    service = (Service)args.gateway;
    append = (uint32_t *)(bt_base + GOBT_MAPPING_APPEND);
    if (gobt_mapping_build_stub(append_stub, (uintptr_t)append_stub, (uintptr_t)mapping_append,
                                bt_base, append) < 0 ||
        gobt_mapping_build_bypass(bypass_stub, (uintptr_t)bypass_stub, bt_base, append[1]) < 0) return -1;
    sceKernelDcacheWritebackAll();
    service(GOBT_MAPPING_FLUSH, NULL);
    patch.abi = GOBT_MAPPING_UI_ABI;
    patch.base = bt_base;
    patch.size = args.size;
    patch.append = (uintptr_t)append_stub;
    patch.bypass = (uintptr_t)bypass_stub;
    return service(GOBT_MAPPING_PATCH, &patch);
}

int module_stop(SceSize size, void *data)
{
    /* Unloaded only after its Bluetooth module, and every hook, is gone. */
    (void)size; (void)data;
    return 0;
}

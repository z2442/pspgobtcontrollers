#include "dualsense.h"
#include "xmb_patch.h"
#include "input_state.h"
#include "probe_policy.h"
#include "hid_route.h"
#include "pairing_route.h"
#include "report_bridge.h"
#include "target_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put_word(uint8_t *image, size_t offset, uint32_t word);

static void sign_input(uint8_t *report)
{
    uint32_t crc = ds_crc32(0xa1, report, 74);
    unsigned i;
    for (i = 0; i < 4; ++i) report[74 + i] = (uint8_t)(crc >> (8 * i));
}

static void set_name(uint8_t *record, const char *name)
{
    unsigned i;
    memset(record + 6, 0, 64);
    for (i = 0; name[i]; ++i) record[6 + 2 * i] = (uint8_t)name[i];
}

static void test_dualshock4(void)
{
    uint8_t report[78] = {0}, output[78], packet[79], info[80] = {0}, scan[84] = {0};
    GoBtReportAssembly assembly = {{0}, 0};
    DsState state, before;
    uint32_t crc;
    unsigned i;
    report[0] = 0x11;
    report[3] = 10; report[4] = 20; report[5] = 30; report[6] = 40;
    report[7] = 0xf1; report[8] = 0xff; report[9] = 1;
    report[10] = 90; report[11] = 180;
    crc = ds_crc32(0xa1, report, 74);
    for (i = 0; i < 4; ++i) report[74 + i] = (uint8_t)(crc >> (8 * i));
    ds_neutral(&state);
    assert(ds4_parse_bt(report, 78, &state) == 0);
    assert(state.lx == 10 && state.ly == 20 && state.rx == 30 && state.ry == 40);
    assert(state.l2 == 90 && state.r2 == 180 && state.buttons == 0x1ff3f);
    before = state;
    report[10] ^= 1;
    assert(ds4_parse_bt(report, 78, &state) == -2 && !memcmp(&state, &before, sizeof(state)));
    report[10] ^= 1;
    report[7] = 15;
    crc = ds_crc32(0xa1, report, 74);
    for (i = 0; i < 4; ++i) report[74 + i] = (uint8_t)(crc >> (8 * i));
    assert(ds4_parse_bt(report, 78, &state) == -3 && !memcmp(&state, &before, sizeof(state)));
    report[7] = 0xf1;
    crc = ds_crc32(0xa1, report, 74);
    for (i = 0; i < 4; ++i) report[74 + i] = (uint8_t)(crc >> (8 * i));
    packet[0] = 0xa1; memcpy(packet + 1, report, 78);
    assert(gobt_decode_hidp(&assembly, packet, 79, &state) == 1);
    assert(gobt_decode_hidp(&assembly, packet, 64, &state) == 0);
    assert(gobt_decode_hidp(&assembly, report + 63, 15, &state) == 1);
    ds4_build_enable(output);
    assert(output[0] == 0x11 && output[1] == 0xc4 && output[3] == 2 && output[10] == 0x40);
    crc = ds_crc32(0xa2, output, 74);
    for (i = 0; i < 4; ++i) assert(output[74 + i] == (uint8_t)(crc >> (8 * i)));
    put_word(info, 0, 80);
    set_name(info, "Wireless Controller");
    info[72] = 2; info[73] = 4;
    assert(gobt_controller_model(info, 80, 80) == GOBT_MODEL_DUALSHOCK4);
    assert(gobt_input_metadata(info, 80));
    assert(gobt_input_identity(info, 80, 0x054c, 0x05c4));
    assert(gobt_input_identity(info, 80, 0x054c, 0x09cc));
    assert(!gobt_input_identity(info, 80, 0x054c, 0x0ce6));
    put_word(scan, 0, 84);
    set_name(scan, "Wireless Controller");
    assert(gobt_pair_scan_record(scan, 84));
    info[6 + 2 * 19] = 'X';
    assert(!gobt_input_metadata(info, 80));
    scan[6 + 2 * 19] = 'X';
    assert(!gobt_pair_scan_record(scan, 84));
}

static void test_protocol(void)
{
    /* Fixed fixture CRC generated independently with Python zlib.crc32.
     * Axes at extremes, all face/shoulder buttons, NE hat and PS. */
    uint8_t input[78] = {
        0x31, 0x15, 0, 255, 65, 192, 127, 254, 0, 0xf1, 0xff, 7,
        [74] = 0xcf, 0xf4, 0xf3, 0x03
    };
    static const uint8_t expected_pressure[12] = {
        255, 255, 0, 0, 127, 254, 255, 255, 255, 255, 255, 255
    };
    static const uint8_t expected_hats[9] = {
        0x10, 0x30, 0x20, 0x60, 0x40, 0xc0, 0x80, 0x90, 0
    };
    DsState state, before;
    uint8_t ds3[49], output[78];
    size_t size;
    unsigned i;
    assert(ds_crc32(0xa1, (const uint8_t *)"123456789", 9) == 0x88ed2411);
    ds_neutral(&state);
    assert(ds_parse_bt(input, sizeof(input), &state) == 0);
    assert(state.buttons == 0x1ff3f);
    assert(state.lx == 0 && state.ly == 255 && state.rx == 65 && state.ry == 192);
    assert(state.l2 == 127 && state.r2 == 254);
    ds_to_ds3(&state, ds3);
    assert(ds3[0] == 1 && ds3[2] == 0x3f && ds3[3] == 0xff && ds3[4] == 1);
    assert(ds3[6] == 0 && ds3[7] == 255 && ds3[8] == 65 && ds3[9] == 192);
    assert(!memcmp(ds3 + 14, expected_pressure, sizeof(expected_pressure)));
    before = state;
    for (size = 0; size < sizeof(input); ++size) {
        assert(ds_parse_bt(input, size, &state) < 0);
        assert(!memcmp(&before, &state, sizeof(state)));
    }
    assert(ds_parse_bt(input, 79, &state) < 0);
    assert(ds_parse_bt(NULL, 78, &state) < 0);
    assert(ds_parse_bt(input, 78, NULL) < 0);
    /* Every one-bit corruption must be rejected, without changing state. */
    for (i = 0; i < 78 * 8; ++i) {
        input[i / 8] ^= (uint8_t)(1u << (i % 8));
        assert(ds_parse_bt(input, 78, &state) < 0);
        assert(!memcmp(&before, &state, sizeof(state)));
        input[i / 8] ^= (uint8_t)(1u << (i % 8));
    }
    memset(input, 0, sizeof(input)); input[0] = 0x31;
    input[2] = input[3] = input[4] = input[5] = 128;
    for (i = 0; i < 16; ++i) {
        input[9] = (uint8_t)i;
        sign_input(input);
        if (i <= 8) {
            assert(ds_parse_bt(input, 78, &state) == 0);
            assert(state.buttons == expected_hats[i]);
        } else {
            assert(ds_parse_bt(input, 78, &state) == -3);
        }
    }
    /* Individually verify buttons so swaps cannot hide in an all-pressed fixture. */
    {
        static const uint8_t expected_bits[8] = {10, 11, 8, 9, 0, 3, 1, 2};
        input[9] = 8;
        for (i = 0; i < 8; ++i) {
            input[10] = (uint8_t)(1u << i);
            sign_input(input);
            assert(ds_parse_bt(input, 78, &state) == 0);
            assert(state.buttons == (1u << expected_bits[i]));
        }
        input[10] = 0;
        for (i = 0; i < 4; ++i) {
            input[9] = (uint8_t)(8 | (0x10u << i));
            sign_input(input);
            assert(ds_parse_bt(input, 78, &state) == 0);
            assert(state.buttons == (1u << (15 - i)));
        }
        input[9] = 8;
        for (i = 0; i < 3; ++i) {
            input[11] = (uint8_t)(1u << i);
            sign_input(input);
            assert(ds_parse_bt(input, 78, &state) == 0);
            assert(state.buttons == (i == 0 ? 1u << 16 : 0));
        }
    }
    ds_neutral(&state);
    ds_to_ds3(&state, ds3);
    assert(ds3[2] == 0 && ds3[3] == 0 && ds3[4] == 0);
    assert(ds3[6] == 128 && ds3[7] == 128 && ds3[8] == 128 && ds3[9] == 128);
    for (i = 14; i < 26; ++i) assert(ds3[i] == 0);
    ds_build_enable(output, 3);
    assert(output[0] == 0x31 && output[1] == 0x30 && output[2] == 0x10);
    assert(output[74] == 0x5a && output[75] == 0x60 && output[76] == 0x94 && output[77] == 0xa2);
    ds_build_enable(output, 0x23);
    assert(output[1] == 0x30);
}

static void put_word(uint8_t *image, size_t offset, uint32_t word)
{
    memcpy(image + offset, &word, sizeof(word));
}

static void test_patch(void)
{
    static uint8_t image[0x13d70], saved[0x13d70];
    uint32_t word;
    put_word(image, 0x13d40, 0x240e0005);
    put_word(image, 0x13d6c, 0x104e0003);
    memcpy(saved, image, sizeof(image));
    assert(gobt_patch_filter(0, image, sizeof(image)) == -1);
    assert(gobt_patch_filter(0x06060110, NULL, sizeof(image)) == -2);
    assert(gobt_patch_filter(0x06060110, image, sizeof(image) - 1) == -2);
    assert(!memcmp(image, saved, sizeof(image)));
    put_word(image, 0x13d6c, 0);
    memcpy(saved, image, sizeof(image));
    assert(gobt_patch_filter(0x06060110, image, sizeof(image)) == -3);
    assert(!memcmp(image, saved, sizeof(image)));
    put_word(image, 0x13d6c, 0x104e0003);
    assert(gobt_patch_filter(0x06060110, image, sizeof(image)) == 0);
    memcpy(&word, image + 0x13d40, 4); assert(word == 0x240effff);
    memcpy(&word, image + 0x13d6c, 4); assert(word == 0x144e0003);
    assert(gobt_patch_filter(0x06060110, image, sizeof(image)) == 1);
    /* Test the second explicitly supported firmware on a fresh image. */
    put_word(image, 0x13d40, 0x240e0005);
    put_word(image, 0x13d6c, 0x104e0003);
    assert(gobt_patch_filter(0x06060010, image, sizeof(image)) == 0);
}

static void test_native_frames(void)
{
    GoBtInput input;
    GoBtCtrlData2 frame;
    DsState state;
    const uint64_t start = 0x100000000ull + 100;
    memset(&frame, 0xa5, sizeof(frame));
    frame.timestamp = 0x12345678;
    gobt_input_reset(&input);
    assert(gobt_input_sample(&input, start, &frame) == 0);
    assert(frame.timestamp == 0x12345678 && frame.buttons == 0);
    assert(frame.lx == 128 && frame.ly == 128 && frame.rx == 128 && frame.ry == 128);
    assert(frame.reserved[0] == 128 && frame.reserved[1] == 128);
    assert(frame.reserved[2] == 0 && frame.reserved[3] == 0);
    ds_neutral(&state);
    state.buttons = 0x8001ff3f;
    state.lx = 0; state.ly = 255; state.rx = 17; state.ry = 223;
    state.l2 = 23; state.r2 = 173;
    gobt_input_update(&input, &state, start);
    assert(gobt_input_sample(&input, start, &frame) == 1);
    assert(frame.buttons == 0x1ff3f);
    assert(frame.lx == 0 && frame.ly == 255 && frame.rx == 17 && frame.ry == 223);
    assert(frame.dpad_a == 255 && frame.dpad_b == 255);
    assert(frame.face_a == 0x00ff00ff && frame.face_b == 0x00ff00ff);
    assert(frame.shoulder == 0x00ff00ff && frame.trigger == 0x00ad0017);
    assert(frame.tilt_a == 0 && frame.tilt_b == 0);
    assert(gobt_input_sample(&input, start + GOBT_INPUT_TIMEOUT_US - 1, &frame) == 1);
    assert(gobt_input_sample(&input, start + GOBT_INPUT_TIMEOUT_US, &frame) == 0);
    assert(frame.buttons == 0 && frame.trigger == 0 && frame.lx == 128);
    assert(frame.timestamp == 0x12345678);
    /* Expired state must not reappear after long idle times or clock changes. */
    assert(gobt_input_sample(&input, start + 0x100000000ull, &frame) == 0);
    gobt_input_update(&input, &state, start);
    assert(gobt_input_sample(&input, start - 1, &frame) == 0);
    assert(gobt_input_sample(&input, start, &frame) == 0);
    gobt_input_update(&input, &state, start);
    gobt_input_reset(&input); /* Disconnect/suspend. */
    assert(gobt_input_sample(&input, start, &frame) == 0);
    assert(frame.buttons == 0 && frame.rx == 128);
    gobt_input_update(&input, &state, start + 1);
    assert(gobt_input_sample(&input, start + 2, &frame) == 1);
}

static void test_probe_guards(void)
{
    static const char *const capture[] = {
        "sceBSMan", "sceUsbBSMCDC_Driver", "sceUsbHost_Driver", "sceUSB_Host_Driver", "sceController_Service",
        "sceHIDSvc", "scePadSvc", "sceInput_Library", "sceBtdun_Driver",
        "bluetooth_plugin_module", "sysconf_plugin_module", "sceVshNetconf_bt_Module"
    };
    static const struct { size_t offset; uint32_t word; } observed[] = {
        {0x150b0, 0x27bdffa0}, {0x150ec, 0x24020007}, {0x150f0, 0x12420004},
        {0x150f4, 0x3c10802f}, {0x150f8, 0x97a20048}, {0x150fc, 0x1040000e},
        {0x15100, 0x36100135}, {0x153f0, 0x24020050}, {0x153f4, 0x03e00008},
        {0x153f8, 0xac820000}
    };
    static uint8_t text[0x15400];
    unsigned i;
    for (i = 0; i < sizeof(capture) / sizeof(capture[0]); ++i)
        assert(gobt_capture_module(capture[i]));
    assert(!gobt_capture_module(NULL));
    assert(!gobt_capture_module("sceAmctrl_driver"));
    assert(!gobt_capture_module("PSPGoBT_Probe"));
    assert(!gobt_capture_module("sceAudio_Driver"));
    assert(!gobt_device_info_trace_supported(0x06060010, text, sizeof(text)));
    for (i = 0; i < sizeof(observed) / sizeof(observed[0]); ++i)
        put_word(text, observed[i].offset, observed[i].word);
    assert(gobt_device_info_trace_supported(0x06060010, text, sizeof(text)));
    assert(gobt_device_info_trace_supported(0x06060110, text, sizeof(text)));
    assert(!gobt_device_info_trace_supported(0, text, sizeof(text)));
    assert(!gobt_device_info_trace_supported(0x06060010, NULL, sizeof(text)));
    assert(!gobt_device_info_trace_supported(0x06060010, text, sizeof(text) - 1));
    /* No individual ABI/gate signature may be weakened without rejecting the hook. */
    for (i = 0; i < sizeof(observed) / sizeof(observed[0]); ++i) {
        put_word(text, observed[i].offset, observed[i].word ^ 1);
        assert(!gobt_device_info_trace_supported(0x06060010, text, sizeof(text)));
        put_word(text, observed[i].offset, observed[i].word);
    }
    {
        static const struct { uint32_t nid; uint32_t words[4]; size_t count; } signatures[] = {
            {0x8ac2f7b8, {0x27bdfff0, 0x24860054, 0x00c41025, 0xafb00000}, 4},
            {0x2d649923, {0x24870001, 0x001b12c0, 0x00e43025, 0x27bdffa0}, 4},
            {0xf188a405, {0x27bdfff0, 0x00a01021, 0x24060402, 0x24050202}, 4},
            {0x68327a27, {0x27bdfff0, 0xafb00000, 0x24050002, 0x03608021}, 4},
            {0x1238f36e, {0x2483ffff, 0x27bdfff0, 0x2cc20001, 0x2c6a0008}, 4}
        };
        uint8_t code[16] = {0};
        unsigned j;
        for (i = 0; i < sizeof(signatures) / sizeof(signatures[0]); ++i) {
            for (j = 0; j < signatures[i].count; ++j)
                put_word(code, 4 * j, signatures[i].words[j]);
            assert(gobt_bsman_api_supported(signatures[i].nid, code, sizeof(code)));
            assert(!gobt_bsman_api_supported(signatures[i].nid, code, sizeof(code) - 1));
            for (j = 0; j < signatures[i].count; ++j) {
                code[4 * j] ^= 1;
                assert(!gobt_bsman_api_supported(signatures[i].nid, code, sizeof(code)));
                code[4 * j] ^= 1;
            }
        }
        memset(code, 0, sizeof(code));
        put_word(code, 8, 0x27bdff90);
        assert(gobt_bsman_api_supported(0xcbd232d1, code, sizeof(code)));
        assert(!gobt_bsman_api_supported(0xcbd232d1, code, 11));
        assert(!gobt_bsman_api_supported(0xcbd232d1, NULL, 16));
        assert(!gobt_bsman_api_supported(0, code, 16));
    }
}

static void test_hid_routing(void)
{
    static const char name[] = "DualSense Wireless Controller";
    uint8_t profile[44], saved[44], info[80] = {0}, original_info[80];
    uint32_t code[GOBT_ROUTE_CODE_WORDS], saved_code[GOBT_ROUTE_CODE_WORDS];
    unsigned i;
    put_word(info, 0, 80);
    for (i = 0; i < sizeof(name) - 1; ++i) info[6 + i * 2] = (uint8_t)name[i];
    info[72] = 2; info[73] = 4;
    memset(profile, 0xa5, sizeof(profile));
    put_word(profile, 4, 2);
    memcpy(saved, profile, sizeof(saved));
    memcpy(original_info, info, sizeof(info));
    assert(gobt_route_hid_record(profile, 44, info, 80) == 1);
    for (i = 0; i < sizeof(profile); ++i)
        assert(profile[i] == (i == 4 ? 9 : i > 4 && i < 8 ? 0 : saved[i]));
    assert(!memcmp(info, original_info, sizeof(info)));
    assert(gobt_route_hid_record(profile, 44, info, 80) == 1); /* Reattach methods for the custom mode. */
    set_name(info, "Wireless Controller");
    memcpy(profile, saved, sizeof(profile));
    assert(gobt_route_hid_record(profile, 44, info, 80) == 1);
    assert(profile[4] == GOBT_PROFILE_GENERIC_HID);
    set_name(info, name);
    memcpy(profile, saved, sizeof(profile));
    assert(!gobt_route_hid_record(profile, 43, info, 80));
    assert(!gobt_route_hid_record(profile, 44, info, 79));
    assert(!gobt_route_hid_record(NULL, 44, info, 80));
    assert(!gobt_route_hid_record(profile, 44, NULL, 80));
    /* Mismatched name, Unicode high byte, size, capability or existing type
     * must leave the whole profile unchanged. */
    {
        const unsigned changed_offsets[] = {0, 6, 7, 24, 72, 73};
        for (i = 0; i < sizeof(changed_offsets) / sizeof(changed_offsets[0]); ++i) {
            unsigned off = changed_offsets[i];
            info[off] ^= 1;
            assert(!gobt_route_hid_record(profile, 44, info, 80));
            assert(!memcmp(profile, saved, sizeof(saved)));
            info[off] ^= 1;
        }
    }
    put_word(profile, 4, 10);
    assert(!gobt_route_hid_record(profile, 44, info, 80));
    memset(code, 0xa5, sizeof(code)); memcpy(saved_code, code, sizeof(code));
    assert(gobt_build_route_code(code, 0x0a002000, 0x88003000, 0x0a004000, 0x0a005000) == -2);
    assert(!memcmp(code, saved_code, sizeof(code)));
    assert(gobt_build_route_code(code, 0x0a002001, 0x0a003000, 0x0a004000, 0x0a005000) == -1);
    assert(gobt_build_route_code(code, 0x0a002000, 0x0a003000, 0x0a004000, 0x0a005000) == 0);
    assert(code[0] == 0x27bdffe0 && code[13] == 0x27bd0020);
    assert((code[4] >> 26) == 3 && ((code[4] & 0x03ffffffu) << 2) == 0x0a003000);
    assert((code[19] >> 26) == 2 && ((code[19] & 0x03ffffffu) << 2) == 0x0a004000);
}

static void test_hid_route_image_guard(void)
{
    static uint8_t image[0x20c40];
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0x14e0c, 0x27bdffe0}, {0x14e14, 0x24020003},
        {0x14e2c, 0x8e110004}, {0x14e30, 0x1222002f},
        {0x14e38, 0x94a20048}, {0x14e40, 0x3042000c},
        {0x14eb8, 0x1622fff8}, {0x14ebc, 0x24040004},
        {0x15124, 0xae320004}, {0x15128, 0x02202021},
        {0x15130, 0x03a02821},
        {0x14f64, 0x27bdffe0}, {0x14f68, 0x2ca20009},
        {0x14f80, 0x00051080}, {0x14f88, 0x00431021},
        {0x14f8c, 0x8c440000}, {0x14f90, 0x00800008},
        {0x14fd4, 0x24040004}, {0x14fe0, 0xac430000},
        {0x14fe4, 0x2604001c}, {0x14fe8, 0x27a50008},
        {0x14ff0, 0xafa20008},
        {0x15514, 0x27bdfff0}, {0x15518, 0x00a02021},
        {0x15524, 0x00002821}, {0x15534, 0x27bdfff0},
        {0x15540, 0x00a02021},
    };
    const uint32_t bases[] = {0x0a572000, 0x0a571f00};
    unsigned i, j;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        put_word(image, sites[i].offset, sites[i].word);
    for (j = 0; j < sizeof(bases) / sizeof(bases[0]); ++j) {
        uint32_t base = bases[j];
        put_word(image, GOBT_ROUTE_CALL_OFFSET,
                 0x0c000000u | (((base + GOBT_ROUTE_NATIVE_OFFSET) >> 2) & 0x03ffffffu));
        put_word(image, 0x1ca54, base + 0x14fd0);
        put_word(image, 0x14fd0, 0x0c000000u | (((base + 0x1abe0) >> 2) & 0x03ffffffu));
        put_word(image, 0x14fec, 0x08000000u | (((base + 0x14fb8) >> 2) & 0x03ffffffu));
        put_word(image, 0x14f7c, 0x3c030000u | (((base + 0x1ca4c + 0x8000u) >> 16) & 0xffffu));
        put_word(image, 0x14f84, 0x24630000u | ((base + 0x1ca4c) & 0xffffu));
        put_word(image, 0x14fd8, 0x3c030000u | (((base + 0x20c28 + 0x8000u) >> 16) & 0xffffu));
        put_word(image, 0x14fdc, 0x24630000u | ((base + 0x20c28) & 0xffffu));
        put_word(image, 0x20c28, base + 0x1a8f4);
        put_word(image, 0x20c2c, base + 0x1a904);
        put_word(image, 0x20c30, base + 0x15514);
        put_word(image, 0x20c34, base + 0x15534);
        assert(gobt_hid_route_supported(0x06060010, image, sizeof(image), base));
        assert(gobt_hid_route_supported(0x06060110, image, sizeof(image), base));
        assert(!gobt_hid_route_supported(0x06060010, image, sizeof(image), base + 4));
        assert(!gobt_hid_route_supported(0x06060010, image, sizeof(image), base + 1));
        for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
            image[sites[i].offset] ^= 1;
            assert(!gobt_hid_route_supported(0x06060010, image, sizeof(image), base));
            image[sites[i].offset] ^= 1;
        }
        image[GOBT_ROUTE_CALL_OFFSET] ^= 1;
        assert(!gobt_hid_route_supported(0x06060010, image, sizeof(image), base));
        image[GOBT_ROUTE_CALL_OFFSET] ^= 1;
        assert(!gobt_hid_route_supported(0x06060010, image, sizeof(image) - 1, base));
        assert(!gobt_hid_route_supported(0x06060010, NULL, sizeof(image), base));
        assert(!gobt_hid_route_supported(0x06050010, image, sizeof(image), base));
    }
}

static void test_pairing_record(void)
{
    static const char name[] = "DualSense Wireless Controller";
    uint8_t record[84] = {0}, saved[84];
    unsigned i;
    put_word(record, 0, 84);
    for (i = 0; i < sizeof(name) - 1; ++i) record[6 + 2 * i] = (uint8_t)name[i];
    memcpy(saved, record, sizeof(saved));
    assert(gobt_pair_scan_record(record, sizeof(record)));
    assert(!memcmp(record, saved, sizeof(record)));
    for (i = 0; i < sizeof(record); ++i)
        assert(!gobt_pair_scan_record(record, i));
    assert(!gobt_pair_scan_record(NULL, 84));
    {
        const unsigned rejected[] = {0, 1, 2, 3, 6, 7, 22, 23, 24, 25};
        for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
            record[rejected[i]] ^= 1;
            assert(!gobt_pair_scan_record(record, sizeof(record)));
            record[rejected[i]] ^= 1;
        }
    }
    /* A bare DualSense name and a suffixed Edge name use the same route. */
    record[24] = 0;
    assert(gobt_pair_scan_record(record, 84));
    memcpy(record, saved, sizeof(record));
    record[26] = 'E'; record[28] = 'd'; record[30] = 'g'; record[32] = 'e'; record[34] = 0;
    assert(gobt_pair_scan_record(record, 84));
    /* The registered-device record is a different interface and is refused. */
    put_word(record, 0, 80);
    assert(!gobt_pair_scan_record(record, 84));
    {
        uint8_t parameter[40], before[40];
        memset(parameter, 0xa5, sizeof(parameter));
        memcpy(before, parameter, sizeof(before));
        assert(gobt_pair_default_parameter(NULL, 34) == -1);
        for (i = 0; i < 34; ++i) {
            assert(gobt_pair_default_parameter(parameter, i) == -1);
            assert(!memcmp(parameter, before, sizeof(parameter)));
        }
        assert(!gobt_pair_default_parameter(parameter, sizeof(parameter)));
        for (i = 0; i < sizeof(parameter); ++i)
            assert(parameter[i] == (i >= 34 ? 0xa5 : i < 8 && !(i & 1) ? '0' : 0));
    }
}

static void test_pairing_image_guard(void)
{
    static uint8_t image[0x13b3c];
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0x13934, 0x27bdfff0}, {0x13938, 0xafbf0000},
        {0x13940, 0x2485004f}, {0x13944, 0x8fbf0000},
        {0x13948, 0x03e00008}, {0x1394c, 0x27bd0010},
        {0x13b28, 0x27bdfff0}, {0x13b2c, 0x24860058},
        {0x13b30, 0xafbf0000}, {0x13b38, 0x24850004},
        {0x138c8, 0x27bdfff0}, {0x138cc, 0x00a02021},
        {0x138d8, 0x00c02821},
        {0x13a5c, 0x24030030}, {0x13a60, 0x24020003},
        {0x13a64, 0x2442ffff}, {0x13a68, 0xa6030000},
        {0x13a6c, 0x0441fffd}, {0x13a70, 0x26100002},
    };
    const uint32_t bases[] = {0x0a572000, 0x0a571f00};
    unsigned i, j;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        put_word(image, sites[i].offset, sites[i].word);
    for (j = 0; j < sizeof(bases) / sizeof(bases[0]); ++j) {
        uint32_t base = bases[j];
        put_word(image, GOBT_PAIR_CALL_OFFSET,
                 0x0c000000u | (((base + GOBT_PAIR_ORIGINAL_OFFSET) >> 2) & 0x03ffffffu));
        assert(gobt_pair_route_supported(0x06060010, image, sizeof(image), base));
        assert(gobt_pair_route_supported(0x06060110, image, sizeof(image), base));
        assert(!gobt_pair_route_supported(0x06060010, image, sizeof(image), base + 4));
        assert(!gobt_pair_route_supported(0x06060010, image, sizeof(image), base + 1));
        for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
            image[sites[i].offset] ^= 1;
            assert(!gobt_pair_route_supported(0x06060010, image, sizeof(image), base));
            image[sites[i].offset] ^= 1;
        }
        image[GOBT_PAIR_CALL_OFFSET] ^= 1;
        assert(!gobt_pair_route_supported(0x06060010, image, sizeof(image), base));
        image[GOBT_PAIR_CALL_OFFSET] ^= 1;
        assert(!gobt_pair_route_supported(0x06060010, image, sizeof(image) - 1, base));
        assert(!gobt_pair_route_supported(0x06060010, NULL, sizeof(image), base));
        assert(!gobt_pair_route_supported(0x06050010, image, sizeof(image), base));
    }
}

static void test_pair_command_guard(void)
{
    static uint8_t image[0x69fc];
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0x3720, 0x27bdffc0}, {0x3724, 0xafb10024},
        {0x3728, 0x3c118824}, {0x372c, 0xafb50034},
        {0x6758, 0x27bdff50}, {0x67e0, 0x24060062},
        {0x67e8, 0x24020022}, {0x688c, 0x02a02021},
        {0x6890, 0x02602821}, {0x6894, 0x00003021},
        {0x68a4, 0xafb20058}, {0x69c0, 0x02a02021},
        {0x69c4, 0x02602821}, {0x69d0, 0x240b0065},
        {0x69d8, 0x02203021}, {0x69f8, 0xafbd0044},
    };
    const unsigned calls[] = {GOBT_PAIR_PARAMETER_CALL_OFFSET, GOBT_PAIR_START_CALL_OFFSET};
    const uint32_t bases[] = {0x88238900, 0x88238800, 0x8820b200};
    unsigned i, j;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        put_word(image, sites[i].offset, sites[i].word);
    for (j = 0; j < sizeof(bases) / sizeof(bases[0]); ++j) {
        uint32_t base = bases[j];
        put_word(image, 0x3728, 0x3c110000u | (((base + 0xdd00u + 0x8000u) >> 16) & 0xffffu));
        put_word(image, 0x3730, 0x26350000u | ((base + 0xdd00u) & 0xffffu));
        for (i = 0; i < 2; ++i)
            put_word(image, calls[i],
                     0x0c000000u | (((base + GOBT_PAIR_COMMAND_OFFSET) >> 2) & 0x03ffffffu));
        assert(gobt_pair_commands_supported(0x06060010, image, sizeof(image), base));
        assert(gobt_pair_commands_supported(0x06060110, image, sizeof(image), base));
        assert(!gobt_pair_commands_supported(0x06060010, image, sizeof(image), base + 4));
        assert(!gobt_pair_commands_supported(0x06060010, image, sizeof(image), base + 1));
        for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
            image[sites[i].offset] ^= 1;
            assert(!gobt_pair_commands_supported(0x06060010, image, sizeof(image), base));
            image[sites[i].offset] ^= 1;
        }
        for (i = 0; i < 2; ++i) {
            image[calls[i]] ^= 1;
            assert(!gobt_pair_commands_supported(0x06060010, image, sizeof(image), base));
            image[calls[i]] ^= 1;
        }
        assert(!gobt_pair_commands_supported(0x06060010, image, sizeof(image) - 1, base));
        assert(!gobt_pair_commands_supported(0x06060010, NULL, sizeof(image), base));
        assert(!gobt_pair_commands_supported(0x06050010, image, sizeof(image), base));
    }
}

static void test_basic_reports_and_hidp(void)
{
    uint8_t simple[10] = {1, 0, 255, 65, 192, 0xf1, 0xff, 0xfd, 127, 254};
    uint8_t full[78] = {0x31, 0, 0, 255, 65, 192, 127, 254, 0, 0xf1, 0xff, 1};
    uint8_t packet[80] = {0xa1}, tail_packet[17] = {0xb1};
    DsState state, baseline, expected;
    GoBtReportAssembly assembly = {{0}, 0};
    unsigned i;
    sign_input(full);
    ds_neutral(&baseline);
    assert(!ds_parse_bt(full, 78, &expected));
    assert(!ds_parse_bt_simple(simple, 10, &state));
    assert(state.buttons == 0x1ff3f && state.buttons == expected.buttons);
    assert(state.lx == 0 && state.ly == 255 && state.rx == 65 && state.ry == 192);
    assert(state.l2 == 127 && state.r2 == 254);
    for (i = 0; i < 10; ++i) {
        state = baseline;
        assert(ds_parse_bt_simple(simple, i, &state) < 0);
        assert(!memcmp(&state, &baseline, sizeof(state)));
    }
    assert(ds_parse_bt_simple(simple, 11, &state) < 0);
    assert(ds_parse_bt_simple(NULL, 10, &state) < 0);
    assert(ds_parse_bt_simple(simple, 10, NULL) < 0);
    simple[5] = 15;
    state = baseline;
    assert(ds_parse_bt_simple(simple, 10, &state) < 0);
    assert(!memcmp(&state, &baseline, sizeof(state)));
    simple[5] = 0xf1;
    memcpy(packet + 1, simple, 10);
    assert(gobt_decode_hidp(&assembly, packet, 11, &state) == 1);
    assert(state.buttons == expected.buttons);
    packet[0] = 0xa2;
    assert(gobt_decode_hidp(&assembly, packet, 11, &state) < 0);
    packet[0] = 0xa1;
    memcpy(packet + 1, full, 78);
    assert(gobt_decode_hidp(&assembly, packet, 79, &state) == 1);
    /* Explicit HIDP continuation: a 64-byte DATA packet then 16-byte DATC. */
    state = baseline;
    assert(gobt_decode_hidp(&assembly, packet, 64, &state) == 0);
    assert(assembly.used == 63 && !memcmp(&state, &baseline, sizeof(state)));
    memcpy(tail_packet + 1, full + 63, 15);
    assert(gobt_decode_hidp(&assembly, tail_packet, 16, &state) == 1);
    assert(!assembly.used && state.buttons == expected.buttons);
    /* The same full report split across raw 64/15-byte USB read completions. */
    assert(gobt_decode_hidp(&assembly, packet, 64, &state) == 0);
    assert(gobt_decode_hidp(&assembly, full + 63, 15, &state) == 1);
    assert(!assembly.used && state.buttons == expected.buttons);
    assert(gobt_decode_hidp(&assembly, packet, 64, &state) == 0);
    {
        uint8_t damaged[15];
        memcpy(damaged, full + 63, 15); damaged[0] ^= 1;
        state = baseline;
        assert(gobt_decode_hidp(&assembly, damaged, 15, &state) == -2);
        assert(!assembly.used && !memcmp(&state, &baseline, sizeof(state)));
    }
    assert(gobt_decode_hidp(&assembly, tail_packet, 16, &state) < 0);
    assert(gobt_decode_hidp(&assembly, packet, 64, &state) == 0);
    tail_packet[15] ^= 1;
    state = baseline;
    assert(gobt_decode_hidp(&assembly, tail_packet, 16, &state) == -2);
    assert(!assembly.used && !memcmp(&state, &baseline, sizeof(state)));
    tail_packet[15] ^= 1;
    assert(gobt_decode_hidp(&assembly, packet, 64, &state) == 0);
    assert(gobt_decode_hidp(&assembly, tail_packet, 17, &state) < 0);
    assert(!assembly.used);
    assert(gobt_decode_hidp(&assembly, packet, 64, &state) == 0);
    assert(gobt_decode_hidp(&assembly, NULL, 1, &state) < 0 && !assembly.used);
    assert(gobt_decode_hidp(&assembly, packet, 80, &state) < 0);
    /* New DATA starts a new report, rather than completing the previous one. */
    assert(gobt_decode_hidp(&assembly, packet, 64, &state) == 0);
    memcpy(packet + 1, simple, 10);
    assert(gobt_decode_hidp(&assembly, packet, 11, &state) == 1 && !assembly.used);
}

static void test_bridge_identity_and_guard(void)
{
    static uint8_t image[0xccc8];
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0x9ee0, 0x27bdffd0}, {0x9ee4, 0xafb3001c}, {0x9ee8, 0x00c09821},
        {0x9efc, 0x8cd20028}, {0x9f00, 0xacc00028}, {0x9f08, 0xae450004},
        {0x9f30, 0xae630028}, {0x9f34, 0x24060040},
        {0x9f38, 0x02604021}, {0x9f3c, 0x8c650008}, {0x9f44, 0x8e640000},
        {0xa21c, 0x3c0f8824}, {0xa220, 0x8e250008},
        {0xa224, 0x25e727e0}, {0xa228, 0x02004021}, {0xa230, 0x24060040},
        {0xa8fc, 0x3c038824}, {0xa900, 0x24626770},
    };
    static const struct { unsigned offset; uint32_t word; } setup[] = {
        {0x7e0, 0x3c038824}, {0x7e4, 0x000430c0},
        {0x7e8, 0x24656600}, {0x7ec, 0x00c51021},
        {0xa128, 0x27bdffe0}, {0xa12c, 0xafbf001c},
        {0xa130, 0xafb20018}, {0xa134, 0xafb10014},
        {0xa138, 0x00808821}, {0xa13c, 0x0e08e438}, {0xa140, 0xafb00010},
        {0xa198, 0x32440400}, {0xa1ac, 0xa6000006},
        {0xa284, 0x02202021},
    };
    const unsigned calls[] = {GOBT_RX_FIRST_CALL_OFFSET, GOBT_RX_NEXT_CALL_OFFSET};
    uint8_t info[80] = {0};
    unsigned i;
    const char *name = "DualSense Wireless Controller";
    put_word(info, 0, 80);
    for (i = 0; name[i]; ++i) info[6 + 2 * i] = (uint8_t)name[i];
    info[72] = 2; info[73] = 4;
    assert(gobt_input_identity(info, 80, 0x054c, 0x0ce6));
    assert(gobt_input_identity(info, 80, 0x054c, 0x0df2));
    assert(!gobt_input_identity(info, 80, 0x054c, 0x0268));
    assert(!gobt_input_identity(info, 80, 0x9999, 0x0ce6));
    assert(!gobt_input_identity(info, 79, 0x054c, 0x0ce6));
    assert(!gobt_input_identity(NULL, 80, 0x054c, 0x0ce6));
    assert(gobt_input_identity_policy(info, 80, 0, 0x054c, 0x0ce6, 0x202) == 1);
    assert(gobt_input_identity_policy(info, 80, 0x802f0153u, 0, 0, 0x202) == 2);
    assert(!gobt_input_identity_policy(info, 80, 0x802f0153u, 0, 0, 0));
    assert(!gobt_input_identity_policy(info, 80, 0x802f0203u, 0, 0, 0x202));
    assert(!gobt_input_identity_policy(info, 80, 0, 0x9999, 0x0ce6, 0x202));
    assert(!gobt_input_identity_policy(info, 79, 0x802f0153u, 0, 0, 0x202));
    {
        const unsigned rejected[] = {0, 6, 7, 24, 25, 72, 73};
        for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
            info[rejected[i]] ^= 1;
            assert(!gobt_input_identity(info, 80, 0x054c, 0x0ce6));
            assert(!gobt_input_identity_policy(info, 80, 0x802f0153u, 0, 0, 0x202));
            info[rejected[i]] ^= 1;
        }
    }
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        put_word(image, sites[i].offset, sites[i].word);
    for (i = 0; i < 2; ++i)
        put_word(image, calls[i], 0x0c000000u | ((0x88238900u + GOBT_RX_SUBMIT_OFFSET) >> 2 & 0x03ffffffu));
    assert(gobt_report_bridge_supported(0x06060010, image, sizeof(image), 0x88238900));
    assert(gobt_report_bridge_supported(0x06060110, image, sizeof(image), 0x88238900));
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        image[sites[i].offset] ^= 1;
        assert(!gobt_report_bridge_supported(0x06060010, image, sizeof(image), 0x88238900));
        image[sites[i].offset] ^= 1;
    }
    for (i = 0; i < 2; ++i) {
        image[calls[i]] ^= 1;
        assert(!gobt_report_bridge_supported(0x06060010, image, sizeof(image), 0x88238900));
        image[calls[i]] ^= 1;
    }
    assert(!gobt_report_bridge_supported(0x06060010, image, sizeof(image), 0x88238800));
    assert(!gobt_report_bridge_supported(0x06060010, image, sizeof(image) - 1, 0x88238900));
    assert(!gobt_report_bridge_supported(0x06060010, NULL, sizeof(image), 0x88238900));
    assert(!gobt_report_bridge_supported(0x06050010, image, sizeof(image), 0x88238900));
    for (i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i)
        put_word(image, setup[i].offset, setup[i].word);
    assert(gobt_input_setup_supported(image, sizeof(image), 0x88238900));
    for (i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i) {
        image[setup[i].offset] ^= 1;
        assert(!gobt_input_setup_supported(image, sizeof(image), 0x88238900));
        image[setup[i].offset] ^= 1;
    }
    assert(!gobt_input_setup_supported(NULL, sizeof(image), 0x88238900));
    assert(!gobt_input_setup_supported(image, 0xa2bf, 0x88238900));
}

static void test_cdc_pending_read_guard(void)
{
    static uint8_t image[0x1640];
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0x1588, 0x00114840}, {0x158c, 0x01314021}, {0x1590, 0x00083080},
        {0x15a0, 0x8c43007c}, {0x15a8, 0xac530084},
        {0x1634, 0xac54007c}, {0x163c, 0xac500080},
        {0x1574, 0x3c168823}, {0x1594, 0x26c78aa0},
    };
    unsigned i;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        put_word(image, sites[i].offset, sites[i].word);
    assert(gobt_cdc_read_state_supported(image, sizeof(image), 0x88226700));
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        image[sites[i].offset] ^= 1;
        assert(!gobt_cdc_read_state_supported(image, sizeof(image), 0x88226700));
        image[sites[i].offset] ^= 1;
    }
    assert(!gobt_cdc_read_state_supported(image, sizeof(image), 0x88226800));
    assert(!gobt_cdc_read_state_supported(image, sizeof(image), 0x88226701));
    assert(!gobt_cdc_read_state_supported(image, sizeof(image) - 1, 0x88226700));
    assert(!gobt_cdc_read_state_supported(NULL, sizeof(image), 0x88226700));
}

static void test_hid_discovery_guard(void)
{
    static uint8_t image[0xcc00];
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0xa1e4, 0x0e08f876}, {0xa1e8, 0x00003821},
        {0xa1fc, 0x0e09153e}, {0xa200, 0x26040008},
        {0xa204, 0x04400018}, {0xa208, 0x00408821},
    };
    unsigned i;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        put_word(image, sites[i].offset, sites[i].word);
    assert(gobt_input_discovery_supported(image, sizeof(image), 0x88238900));
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        image[sites[i].offset] ^= 1;
        assert(!gobt_input_discovery_supported(image, sizeof(image), 0x88238900));
        image[sites[i].offset] ^= 1;
    }
    assert(!gobt_input_discovery_supported(NULL, sizeof(image), 0x88238900));
    assert(!gobt_input_discovery_supported(image, sizeof(image) - 1, 0x88238900));
    assert(!gobt_input_discovery_supported(image, sizeof(image), 0x88238a00));
    assert(!gobt_input_discovery_supported(image, sizeof(image), 0x88238901));
}

static void test_reconnect_guard(void)
{
    static uint8_t image[0x2b30];
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0x18d0, 0x0e090a8a}, {0x18d4, 0x96040004}, {0x18d8, 0x0a08e802},
        {0x2028, 0x0e090a8a}, {0x202c, 0x8e24000c}, {0x2030, 0x0443ff18},
        {0x2034, 0x8e240008}, {0x2038, 0x8e24000c}, {0x203c, 0x0a08e9b2},
        {0x2040, 0x24050002}, {0x1dc8, 0x0e08ed04}, {0x1dcc, 0x00003021},
        {0x2b10, 0x27bdff90}, {0x2b14, 0x3c028824},
        {0x2b18, 0xafb40060}, {0x2b1c, 0x00a0a021}, {0x2b2c, 0x245253c0},
    };
    unsigned i;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        put_word(image, sites[i].offset, sites[i].word);
    assert(gobt_reconnect_supported(image, sizeof(image), 0x88238900));
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        image[sites[i].offset] ^= 1;
        assert(!gobt_reconnect_supported(image, sizeof(image), 0x88238900));
        image[sites[i].offset] ^= 1;
    }
    assert(!gobt_reconnect_supported(NULL, sizeof(image), 0x88238900));
    assert(!gobt_reconnect_supported(image, sizeof(image) - 1, 0x88238900));
    assert(!gobt_reconnect_supported(image, sizeof(image), 0x88238a00));
    /* Same Sony image relocated by game startup. Keep every instruction
     * guard, adjusting only addresses encoded by the firmware loader. */
    put_word(image, 0x18d0, 0x0e0854ca);
    put_word(image, 0x2028, 0x0e0854ca);
    put_word(image, 0x18d8, 0x0a083242);
    put_word(image, 0x203c, 0x0a0833f2);
    put_word(image, 0x1dc8, 0x0e083744);
    put_word(image, 0x2b14, 0x3c028821);
    put_word(image, 0x2b2c, 0x24527cc0);
    assert(gobt_reconnect_supported(image, sizeof(image), 0x8820b200));
    image[0x2b2c] ^= 1;
    assert(!gobt_reconnect_supported(image, sizeof(image), 0x8820b200));
}

static void test_hid_reset_guard(void)
{
    static uint8_t image[0xa3ac];
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0xa2dc, 0x27bdffe0}, {0xa2e0, 0xafb3000c},
        {0xa300, 0x00808021}, {0xa30c, 0x3214ffff},
        {0xa31c, 0x24c50008}, {0xa328, 0x9507000c},
        {0xa32c, 0x50f40005}, {0xa330, 0x00659021},
        {0xa358, 0x26440008}, {0xa374, 0x8c440008},
        {0xa388, 0x26440020}, {0xa398, 0x26440018},
        {0xa39c, 0xa6400004}, {0xa3a8, 0xae400000},
    };
    static const struct { unsigned offset; uint32_t target; } calls[] = {
        {0xa354, 0xcbe8}, {0xa370, 0xcc88}, {0xa38c, 0xa3e4}, {0xa394, 0xa3e4},
    };
    const uint32_t bases[] = {0x88238900, 0x8820b200};
    unsigned i, j;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        put_word(image, sites[i].offset, sites[i].word);
    for (j = 0; j < sizeof(bases) / sizeof(bases[0]); ++j) {
        uint32_t base = bases[j], state = base + 0xde70u;
        put_word(image, 0xa2e4, 0x3c130000u | (((state + 0x8000u) >> 16) & 0xffffu));
        put_word(image, 0xa310, 0x26660000u | (state & 0xffffu));
        for (i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i)
            put_word(image, calls[i].offset, 0x0c000000u |
                     (((base + calls[i].target) >> 2) & 0x03ffffffu));
        assert(gobt_hid_reset_supported(image, sizeof(image), base));
        assert(!gobt_hid_reset_supported(image, sizeof(image) - 1, base));
        assert(!gobt_hid_reset_supported(image, sizeof(image), base + 4));
        assert(!gobt_hid_reset_supported(NULL, sizeof(image), base));
        for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
            image[sites[i].offset] ^= 1;
            assert(!gobt_hid_reset_supported(image, sizeof(image), base));
            image[sites[i].offset] ^= 1;
        }
        for (i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i) {
            image[calls[i].offset] ^= 1;
            assert(!gobt_hid_reset_supported(image, sizeof(image), base));
            image[calls[i].offset] ^= 1;
        }
    }
}

static void test_cached_record_guard(void)
{
    static uint8_t image[0x43e8];
    static const struct { unsigned offset; uint32_t word; } sites[] = {
        {0x3720, 0x27bdffc0}, {0x3724, 0xafb10024}, {0x372c, 0xafb50034},
        {0x3738, 0x00809821}, {0x3744, 0x00c09021}, {0x3750, 0x00a08021},
        {0x43ac, 0x240c0063}, {0x43b0, 0x240b0002},
        {0x43b4, 0x240a0163}, {0x43b8, 0x24090050},
        {0x43c4, 0x02403021}, {0x43c8, 0xa7b10000},
        {0x43cc, 0xa7ac0010}, {0x43d0, 0xa7ab0012},
        {0x43d4, 0xa7aa0020}, {0x43d8, 0xa7a90022},
        {0x43dc, 0xafb00028}, {0x43e4, 0xafbd0014},
        {0x436c, 0x00043900}, {0x4378, 0x00f31821}, {0x437c, 0x24620010},
        {0x4394, 0x94470004}, {0x43a0, 0x94450006},
        {0x1524, 0x24c7002a}, {0x1538, 0x24d00024},
        {0x15ac, 0xa5090006}, {0x15b0, 0xa5060004},
        {0x15b4, 0xa2220005}, {0x15b8, 0xa2390000},
        {0x15bc, 0xa2380001}, {0x15c0, 0xa2350002},
        {0x15c4, 0xa2340003}, {0x15c8, 0xa22f0004},
    };
    const uint32_t bases[] = {0x88238900, 0x8820b200};
    unsigned i, j;
    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i)
        put_word(image, sites[i].offset, sites[i].word);
    for (j = 0; j < sizeof(bases) / sizeof(bases[0]); ++j) {
        uint32_t base = bases[j], state = base + 0xdd00u;
        put_word(image, 0x3728, 0x3c110000u | (((state + 0x8000u) >> 16) & 0xffffu));
        put_word(image, 0x3730, 0x26350000u | (state & 0xffffu));
        put_word(image, 0x4364, 0x3c020000u | (((state + 0x8000u) >> 16) & 0xffffu));
        put_word(image, 0x4370, 0x24530000u | (state & 0xffffu));
        put_word(image, 0x43e0, 0x0c000000u | (((base + 0x3720u) >> 2) & 0x03ffffffu));
        assert(gobt_cached_record_supported(image, sizeof(image), base));
        assert(!gobt_cached_record_supported(image, sizeof(image) - 1, base));
        assert(!gobt_cached_record_supported(image, sizeof(image), base + 4));
        assert(!gobt_cached_record_supported(NULL, sizeof(image), base));
        for (i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
            image[sites[i].offset] ^= 1;
            assert(!gobt_cached_record_supported(image, sizeof(image), base));
            image[sites[i].offset] ^= 1;
        }
    }
}

static void test_target_cache(void)
{
    GoBtTargetCache cache = {0}, restored;
    uint8_t bytes[GOBT_TARGET_CACHE_BYTES], damaged[GOBT_TARGET_CACHE_BYTES];
    uint8_t first[6] = {1, 2, 3, 4, 5, 6}, second[6] = {7, 8, 9, 10, 11, 12};
    uint8_t third[6] = {13, 14, 15, 16, 17, 18}, invalid[6] = {0};
    unsigned i;
    assert(gobt_target_remember(&cache, invalid) < 0);
    memset(invalid, 255, sizeof(invalid));
    assert(gobt_target_remember(&cache, invalid) < 0);
    assert(gobt_target_remember(&cache, first) == 1);
    assert(gobt_target_remember(&cache, first) == 0);
    assert(gobt_target_remember(&cache, second) == 1);
    assert(gobt_target_encode(&cache, bytes) == 0);
    assert(gobt_target_decode(&restored, bytes, sizeof(bytes)) == 0);
    assert(restored.count == 2 && gobt_target_contains(&restored, first) && gobt_target_contains(&restored, second));
    for (i = 0; i < sizeof(bytes); ++i) {
        memcpy(damaged, bytes, sizeof(bytes));
        damaged[i] ^= 1;
        assert(gobt_target_decode(&restored, damaged, sizeof(damaged)) < 0 && !restored.count);
    }
    assert(gobt_target_decode(&restored, bytes, sizeof(bytes) - 1) < 0);
    assert(gobt_target_remember(&cache, third) == 1);
    assert(!gobt_target_contains(&cache, first) && gobt_target_contains(&cache, second) && gobt_target_contains(&cache, third));
}

int main(void)
{
    test_dualshock4();
    test_target_cache();
    test_hid_reset_guard();
    test_cached_record_guard();
    test_reconnect_guard();
    test_hid_discovery_guard();
    test_protocol();
    test_patch();
    test_native_frames();
    test_probe_guards();
    test_hid_routing();
    test_hid_route_image_guard();
    test_pairing_record();
    test_pairing_image_guard();
    test_pair_command_guard();
    test_basic_reports_and_hidp();
    test_bridge_identity_and_guard();
    test_cdc_pending_read_guard();
    puts("protocol fixtures, corruption rejection, native frames, timeout/disconnect release and guarded patches: PASS");
    return 0;
}

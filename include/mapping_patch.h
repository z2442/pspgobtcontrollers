#ifndef PSPGOBT_MAPPING_PATCH_H
#define PSPGOBT_MAPPING_PATCH_H
#include <stddef.h>
#include <stdint.h>
/* Offsets in the 6.60/6.61 bluetooth_plugin_module text image. */
#define GOBT_MAPPING_APPEND 0x10a80u      /* After the device menu's last native Add. */
#define GOBT_MAPPING_GENERIC_JUMP 0x10bb8u /* Generic option path: j APPEND+4. */
#define GOBT_MAPPING_TEXT 0x11e4u
#define GOBT_MAPPING_STUB_WORDS 13
#define GOBT_MAPPING_BYPASS_WORDS 3
#define GOBT_MAPPING_WIDE_CTOR 0x15cb4u
#define GOBT_MAPPING_VECTOR_PUSH 0x16cc0u
#define GOBT_MAPPING_WIDE_DTOR 0x158a0u
#define GOBT_MAPPING_CLEAR 0x67d0u        /* Native item reset used on close. */
#define GOBT_MAPPING_OPEN 0x9dd4u         /* open(menu, parent, name, 0) */
#define GOBT_MAPPING_CLOSE_DONE 0x91ccu   /* Close completion: clear, notify, idle. */
#define GOBT_MAPPING_VTABLE 0x1fd58u      /* Option menu class. */
#define GOBT_MAPPING_CLOSE_SLOT 0x8cu
#define GOBT_MAPPING_MENU_FLAGS 0x2cu     /* Bit 2: closing. */
#define GOBT_MAPPING_MENU_PAGE 0x08u      /* Attached page, zero after detach. */
int gobt_mapping_supported(uint32_t firmware, const uint8_t *text, size_t size, uint32_t base);
int gobt_mapping_build_stub(uint32_t *code, uint32_t address, uint32_t callback,
                            uint32_t base, const uint32_t displaced[2]);
/* Generic option menus jump directly to APPEND+4. Replay that displaced
 * instruction for them, then resume at APPEND+8, never entering the hook. */
int gobt_mapping_build_bypass(uint32_t *code, uint32_t address, uint32_t base,
                              uint32_t displaced);
#endif

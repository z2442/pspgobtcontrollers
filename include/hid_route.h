#ifndef PSPGOBT_HID_ROUTE_H
#define PSPGOBT_HID_ROUTE_H
#include <stddef.h>
#include <stdint.h>

#define GOBT_PROFILE_BYTES 44u
#define GOBT_DEVICE_INFO_BYTES 80u
#define GOBT_ROUTE_CALL_OFFSET 0x1512cu
#define GOBT_ROUTE_NATIVE_OFFSET 0x14e0cu
#define GOBT_ROUTE_FACTORY_OFFSET 0x14f64u
#define GOBT_ROUTE_CODE_WORDS 21u
#define GOBT_PROFILE_GENERIC_HID 9u
#define GOBT_STORED_CODE_WORDS 29u
#define GOBT_LABEL_CODE_WORDS 12u
#define GOBT_STORE_CODE_WORDS 8u
#define GOBT_STORED_CALL_OFFSET 0x151dcu
#define GOBT_LABEL_ENTRY_OFFSET 0x146c4u
#define GOBT_STORE_ENTRY_OFFSET 0x14d34u

/* Selects the observed DualSense record and sets its generic HID device type.
 * Returns 1 even when already in the custom mode: fresh objects need methods.
 * The display name is a routing guard, not authentication. */
int gobt_route_hid_record(uint8_t *profile, size_t profile_size,
                         const uint8_t *info, size_t info_size);
int gobt_hid_route_supported(uint32_t firmware, const uint8_t *text,
                            size_t size, uint32_t base);
int gobt_hid_mode_supported(const uint8_t *text, size_t size, uint32_t base);
/* Only for a slot independently recognized from the learned address cache. */
int gobt_route_hid_cached(uint8_t *profile, size_t size);
/* Selected objects get Sony's HID methods through its method factory, retain
 * custom type 9. Other objects tail-call the original constructor. */
int gobt_build_route_code(uint32_t code[GOBT_ROUTE_CODE_WORDS], uint32_t address,
                          uint32_t syscall_stub, uint32_t native_function,
                          uint32_t method_factory);
int gobt_build_stored_hid_code(uint32_t code[GOBT_STORED_CODE_WORDS],
                              uint32_t address, uint32_t decision, uint32_t base);
int gobt_build_hid_label_code(uint32_t code[GOBT_LABEL_CODE_WORDS],
                             uint32_t address, uint32_t base);
int gobt_build_hid_store_code(uint32_t code[GOBT_STORE_CODE_WORDS],
                             uint32_t address, uint32_t base);
#endif

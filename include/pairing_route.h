#ifndef PSPGOBT_PAIRING_ROUTE_H
#define PSPGOBT_PAIRING_ROUTE_H
#include <stddef.h>
#include <stdint.h>

#define GOBT_SCAN_RECORD_BYTES 84u
#define GOBT_PAIR_PARAMETER_BYTES 34u
#define GOBT_PAIR_CALL_OFFSET 0x1393cu
#define GOBT_PAIR_ORIGINAL_OFFSET 0x138e8u
#define GOBT_PAIR_COMMAND_OFFSET 0x3720u
#define GOBT_PAIR_PARAMETER_CALL_OFFSET 0x68a0u
#define GOBT_PAIR_START_CALL_OFFSET 0x69f4u

/* The fallback object contains a vtable followed by an unchanged scan record.
 * Name matching selects a registration route; it does not authenticate peers. */
int gobt_pair_scan_record(const uint8_t *record, size_t size);
int gobt_pair_default_parameter(uint8_t *parameter, size_t size);
int gobt_pair_route_supported(uint32_t firmware, const uint8_t *text,
                              size_t size, uint32_t base);
int gobt_pair_commands_supported(uint32_t firmware, const uint8_t *text,
                                 size_t size, uint32_t base);
#endif

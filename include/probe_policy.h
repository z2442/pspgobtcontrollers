#ifndef PSPGOBT_PROBE_POLICY_H
#define PSPGOBT_PROBE_POLICY_H
#include <stddef.h>
#include <stdint.h>
int gobt_capture_module(const char *name);
/* Matches the observed 6.6x 80-byte device-record caller and rejection gate.
 * This checks an ABI fingerprint, not whether the branch ran on hardware. */
int gobt_device_info_trace_supported(uint32_t firmware, const uint8_t *text, size_t size);
int gobt_bsman_api_supported(uint32_t nid, const uint8_t *code, size_t size);
#endif

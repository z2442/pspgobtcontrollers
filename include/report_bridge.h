#ifndef PSPGOBT_REPORT_BRIDGE_H
#define PSPGOBT_REPORT_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#include "dualsense.h"

#define GOBT_RX_CALLBACK_OFFSET 0x9ee0u
#define GOBT_RX_FIRST_CALL_OFFSET 0xa22cu
#define GOBT_RX_NEXT_CALL_OFFSET 0x9f40u
#define GOBT_RX_SUBMIT_OFFSET 0xccc0u
#define GOBT_RX_CONTEXT_OFFSET 0xde78u
#define GOBT_RX_IDENTITY_CALL_OFFSET 0xa1e4u
#define GOBT_RX_SERVICE_CALL_OFFSET 0xa1fcu
#define GOBT_SETUP_EVENT_CALL_OFFSET 0x18d0u
#define GOBT_SETUP_CHECKED_CALL_OFFSET 0x2028u
#define GOBT_SETUP_FLAGS_CALL_OFFSET 0xa13cu
#define GOBT_HID_CLOSE_POINTER_OFFSET 0xdabcu
#define GOBT_SESSION_EVENT_CALL_OFFSET 0x0a24u
#define GOBT_CONNECT_POLL_CALL_OFFSET 0x2880u

typedef struct {
    uint8_t report[DS_BT_REPORT_SIZE];
    unsigned used;
} GoBtReportAssembly;

/* HIDP DATA input (A1) and explicit DATC input (B1), exact bounded frames. */
int gobt_decode_hidp(GoBtReportAssembly *assembly, const uint8_t *packet,
                     size_t size, DsState *state);
int gobt_input_identity(const uint8_t *info, size_t size, unsigned vendor, unsigned product);
int gobt_input_metadata(const uint8_t *info, size_t size);
/* Returns 1 for matching numeric identity, 2 for the observed DID-query
 * failure on a connected, named DualSense, or 0 to refuse the source. */
int gobt_input_identity_policy(const uint8_t *info, size_t size, uint32_t identity_result,
                                unsigned vendor, unsigned product, unsigned link_flags);
int gobt_report_bridge_supported(uint32_t firmware, const uint8_t *text,
                                  size_t size, uint32_t base);
int gobt_input_setup_supported(const uint8_t *text, size_t size, uint32_t base);
int gobt_cached_record_supported(const uint8_t *text, size_t size, uint32_t base);
int gobt_input_discovery_supported(const uint8_t *text, size_t size, uint32_t base);
int gobt_reconnect_supported(const uint8_t *text, size_t size, uint32_t base);
int gobt_hid_reset_supported(const uint8_t *text, size_t size, uint32_t base);
int gobt_hid_close_supported(const uint8_t *text, size_t size, uint32_t base);
int gobt_session_event_supported(const uint8_t *text, size_t size, uint32_t base);
int gobt_connect_epoch_supported(const uint8_t *text, size_t size, uint32_t base);
int gobt_cdc_read_state_supported(const uint8_t *text, size_t size, uint32_t base);
#endif

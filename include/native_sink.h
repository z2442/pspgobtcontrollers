#ifndef PSPGOBT_NATIVE_SINK_H
#define PSPGOBT_NATIVE_SINK_H
#include <stddef.h>
#include <stdint.h>
#include "dualsense.h"

/* Explicitly claims native DS3 port 1. Resident until reboot. test_mode runs
 * the local-analog-to-D-pad test; otherwise a transport must submit reports. */
int gobt_sink_start(int test_mode);
/* Normal worker thread only. Reclaims the port after delayed firmware resets,
 * throttled and only while accepted input is fresh. Never called by sampler. */
void gobt_sink_maintain(void);
void gobt_sink_shutdown(void);
/* Internal, already decoded/selected transport path; not a user export. */
int gobt_sink_submit_state(const DsState *state);
/* Kernel-only transport boundary. Caller must have authenticated and selected
 * a DualSense; CRC and report shape do not establish device identity. */
int pspgobtSubmitReport(const uint8_t *report, size_t size);
void pspgobtDisconnect(void);
#endif

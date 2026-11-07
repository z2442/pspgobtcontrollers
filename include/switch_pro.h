#ifndef PSPGOBT_SWITCH_PRO_H
#define PSPGOBT_SWITCH_PRO_H
#include "dualsense.h"

#define SWITCH_PRO_REPORT_SIZE 49u
enum { SWITCH_PRO_MODE, SWITCH_PRO_FACTORY, SWITCH_PRO_USER,
       SWITCH_PRO_LIGHTS, SWITCH_PRO_DONE };
typedef struct {
    uint16_t center, below, above;
} SwitchProAxis;
/* Zero initialization starts a new connection. Calibration is read anew;
 * missing/invalid calibration uses a nominal center and travel. */
typedef struct {
    SwitchProAxis axis[4];
    unsigned step, attempts, sequence, waiting, streaming;
    uint64_t retry_after;
} SwitchPro;

/* Report ID included, HIDP header excluded. Returns 1 for full controls,
 * 0 for a setup reply, -1 for an invalid/unsupported report. No report CRC
 * exists in this protocol: the caller must select the connected model. */
int switch_pro_receive(SwitchPro *pro, const uint8_t *report, size_t size, DsState *state);
/* Builds the next paced, bounded setup attempt, or returns 0 while waiting.
 * Only read-only SPI commands, report-mode selection and player LED control. */
int switch_pro_setup(SwitchPro *pro, uint64_t now, uint8_t report[SWITCH_PRO_REPORT_SIZE]);
#endif

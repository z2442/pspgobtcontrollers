#ifndef PSPGOBT_CTRL_READ_H
#define PSPGOBT_CTRL_READ_H
#include <stdint.h>
#include "dualsense.h"

/* Guarded 6.6x controller-read hooks. Installation/retirement run on a worker. */
int gobt_ctrl_read_install(uintptr_t registration, const void *handler, const void *source);
void gobt_ctrl_read_shutdown(uintptr_t registration);
int gobt_ctrl_read_active(void);
/* Caller holds interrupts; independent user/kernel latch edge state. */
void gobt_ctrl_read_update(const DsState *state);
void gobt_ctrl_read_release(void);
#endif

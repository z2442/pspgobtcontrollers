#ifndef PSPGOBT_BT_INPUT_BRIDGE_H
#define PSPGOBT_BT_INPUT_BRIDGE_H
#include <psploadcore.h>
int gobt_install_input_bridge(SceModule *module, unsigned long firmware);
/* Starts the resident retry worker even when Bluetooth providers load later. */
int gobt_start_input_bridge(unsigned long firmware);
/* Native sampler: bounded decoding of an already selected, initialized stream.
 * Never performs setup, provider calls, logging, or waits. */
void gobt_input_bridge_sample(void);
/* Kernel reboot callbacks: request cooperative retirement; wait only in the
 * before phase, while providers and the scheduler are still available. */
void gobt_shutdown_input_bridge(int wait);
void gobt_input_bridge_connected(unsigned device);
/* Tokenized UI disconnect intent; native cleanup is serialized by the worker. */
unsigned gobt_input_bridge_disconnecting(unsigned device);
void gobt_input_bridge_disconnect_result(unsigned device, unsigned token, int result);
int gobt_input_bridge_connect_ready(unsigned device);
/* Returns 1 for supported controller metadata, 0 for another stored device, or a
 * negative result when the record is not currently available. */
int gobt_input_bridge_prepare(unsigned device);
/* Passive selection for saved XMB objects if their metadata query fails. */
int gobt_input_bridge_known(unsigned device);
#endif

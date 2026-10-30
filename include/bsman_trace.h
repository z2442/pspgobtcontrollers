#ifndef PSPGOBT_BSMAN_TRACE_H
#define PSPGOBT_BSMAN_TRACE_H
#include <psploadcore.h>
int gobt_trace_device_info(SceModule *module, unsigned long firmware);
int gobt_registration_trace_ready(void);
/* Syscall-exported wrapper for the observed two-argument record query. */
int pspgobtTraceDeviceInfo(unsigned int device, void *record);
int pspgobtTraceRegisterScan(const void *record, const void *parameter);
int pspgobtTraceRegisterAddress(const void *address);
int pspgobtTraceConnect(unsigned int device, int mode);
int pspgobtTraceDisconnect(unsigned int device);
int pspgobtTraceIdentity(unsigned int device, void *vendor, void *product, void *extra);
#endif

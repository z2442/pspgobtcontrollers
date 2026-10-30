#ifndef PSPGOBT_XMB_HID_ROUTE_H
#define PSPGOBT_XMB_HID_ROUTE_H
#include <psploadcore.h>
int gobt_install_hid_route(SceModule *module, unsigned long firmware);
int pspgobtRouteHid(void *profile, const void *info);
int pspgobtRouteStoredHid(void *profile, const void *info, unsigned device);
#endif

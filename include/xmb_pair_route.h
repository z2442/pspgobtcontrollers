#ifndef PSPGOBT_XMB_PAIR_ROUTE_H
#define PSPGOBT_XMB_PAIR_ROUTE_H
#include <psploadcore.h>
int gobt_install_pair_route(SceModule *module, unsigned long firmware);
int pspgobtPairFallback(const void *object, const void *address);
#endif

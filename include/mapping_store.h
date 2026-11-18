#ifndef PSPGOBT_MAPPING_STORE_H
#define PSPGOBT_MAPPING_STORE_H
#include <psploadcore.h>
#include "mapping_ui.h"
int gobt_mapping_start(void);
void gobt_mapping_shutdown(void);
int gobt_mapping_closing(void);
void gobt_mapping_install(SceModule *module, unsigned long firmware);
int pspgobtMappingService(unsigned command, void *request);
#endif

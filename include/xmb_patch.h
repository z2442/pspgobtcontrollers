#ifndef PSPGOBT_XMB_PATCH_H
#define PSPGOBT_XMB_PATCH_H
#include <stddef.h>
#include <stdint.h>
/* Returns 0 if applied, 1 if already applied, negative on mismatch.
 * Caller supplies synchronization and instruction/data cache maintenance. */
int gobt_patch_filter(uint32_t firmware, uint8_t *text, size_t size);
#endif

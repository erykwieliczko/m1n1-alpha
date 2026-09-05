/* SPDX-License-Identifier: MIT */

#ifndef __WDT_H__
#define __WDT_H__

#include "types.h"

void wdt_disable(void);
void wdt_reboot(void);

/* Primary watchdog lease helpers. */
void wdt_init(void);
bool wdt_primary_is_active(void);
int wdt_set_timeout_ms(u64 timeout_ms, u32 *effective_ticks);
int wdt_refresh(void);
void wdt_checkpoint(const char *name);

#endif

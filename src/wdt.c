/* SPDX-License-Identifier: MIT */

#include "../build/build_cfg.h"

#include "wdt.h"
#include "adt.h"
#include "soc.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c
#define WDT_RESET_ENABLE BIT(2)

#define J713_WD1_CLOCK_HZ       24000000ULL
#define J713_PRIMARY_TIMEOUT_MS 10000

static u64 wdt_base = 0;
static u64 wdt_clock_hz = 0;
static u32 wdt_alarm_ticks = 0;
static bool primary_wdt_active = false;

static void wdt_barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

#ifndef CHAINLOADING
static bool is_j713_t8132(void)
{
    int chosen = adt_path_offset(adt, "/chosen");
    u32 adt_chip_id = 0;
    const char *target = adt_getprop(adt, 0, "target-type", NULL);

    if (chosen < 0 || ADT_GETPROP(adt, chosen, "chip-id", &adt_chip_id) < 0 ||
        adt_chip_id != T8132)
        return false;

    return (target && (!strcmp(target, "J713AP") || !strcmp(target, "J713"))) ||
           adt_is_compatible(adt, 0, "J713AP");
}
#endif

static int wdt_get_node_and_primary_from_adt(int *node, u64 *base)
{
    int path[8];
    int offset = adt_path_offset_trace(adt, "/arm-io/wdt", path);

    if (offset < 0)
        return -1;

    if (adt_get_reg(adt, path, "reg", 0, base, NULL) != 0)
        return -1;

    if (node)
        *node = offset;
    return 0;
}

#ifndef CHAINLOADING
static u64 wdt_get_clock_from_adt(void)
{
    int node = adt_path_offset(adt, "/arm-io/wdt");
    u32 len = 0;
    const void *prop;

    if (node < 0)
        return 0;

    prop = adt_getprop(adt, node, "clock-frequency", &len);
    if (!prop)
        return 0;

    if (len == sizeof(u32)) {
        u32 clock;
        memcpy(&clock, prop, sizeof(clock));
        return clock;
    }

    if (len == sizeof(u64)) {
        u64 clock;
        memcpy(&clock, prop, sizeof(clock));
        return clock;
    }

    return 0;
}
#endif

static void wdt_disable_auxiliary(int node)
{
    int path[8];
    u32 wdt_version;
    u64 auxiliary;

    if (node < 0 || ADT_GETPROP(adt, node, "wdt-version", &wdt_version) < 0 ||
        (wdt_version != 2 && wdt_version != 3))
        return;

    if (adt_path_offset_trace(adt, "/arm-io/wdt", path) < 0 ||
        adt_get_reg(adt, path, "reg", 2, &auxiliary, NULL) != 0) {
        printf("Failed to locate auxiliary WDT control\n");
        return;
    }

    printf("Auxiliary WDT control @ 0x%lx\n", auxiliary);
    write32(auxiliary, 0);
    wdt_barrier();
    printf("Auxiliary WDT disabled\n");
}

static int wdt_timeout_to_ticks(u64 timeout_ms, u64 clock_hz, u32 *ticks)
{
    u64 value;

    if (!timeout_ms || !clock_hz || !ticks || timeout_ms > UINT64_MAX / clock_hz)
        return -1;

    value = timeout_ms * clock_hz / 1000;
    if (!value || value > UINT32_MAX)
        return -1;

    *ticks = value;
    return 0;
}

int wdt_set_timeout_ms(u64 timeout_ms, u32 *effective_ticks)
{
    u32 ticks;

    if (!primary_wdt_active || !wdt_base)
        return -1;

    if (!timeout_ms) {
        write32(wdt_base + WDT_CTL, 0);
        wdt_barrier();
        if (read32(wdt_base + WDT_CTL) != 0)
            return -1;
        wdt_alarm_ticks = 0;
        if (effective_ticks)
            *effective_ticks = 0;
        return 0;
    }

    if (wdt_timeout_to_ticks(timeout_ms, wdt_clock_hz, &ticks) < 0)
        return -1;

    write32(wdt_base + WDT_CTL, 0);
    write32(wdt_base + WDT_ALARM, ticks);
    write32(wdt_base + WDT_COUNT, 0);
    write32(wdt_base + WDT_CTL, WDT_RESET_ENABLE);
    wdt_barrier();

    if (read32(wdt_base + WDT_ALARM) != ticks ||
        read32(wdt_base + WDT_CTL) != WDT_RESET_ENABLE)
        return -1;

    wdt_alarm_ticks = ticks;
    if (effective_ticks)
        *effective_ticks = ticks;
    return 0;
}

int wdt_refresh(void)
{
    if (!primary_wdt_active || !wdt_base || !wdt_alarm_ticks)
        return -1;

    if (read32(wdt_base + WDT_ALARM) != wdt_alarm_ticks ||
        read32(wdt_base + WDT_CTL) != WDT_RESET_ENABLE) {
        u32 ticks = wdt_alarm_ticks;

        write32(wdt_base + WDT_CTL, 0);
        write32(wdt_base + WDT_ALARM, ticks);
        write32(wdt_base + WDT_COUNT, 0);
        write32(wdt_base + WDT_CTL, WDT_RESET_ENABLE);
    } else {
        write32(wdt_base + WDT_COUNT, 0);
    }

    wdt_barrier();
    return read32(wdt_base + WDT_ALARM) == wdt_alarm_ticks &&
                   read32(wdt_base + WDT_CTL) == WDT_RESET_ENABLE
               ? 0
               : -1;
}

void wdt_checkpoint(const char *name)
{
    if (!primary_wdt_active)
        return;

    if (wdt_refresh() < 0)
        panic("WD1 refresh failed at checkpoint: %s\n", name);

    printf("WD1 checkpoint: %s\n", name);
}

void wdt_init(void)
{
    int node;

    if (wdt_get_node_and_primary_from_adt(&node, &wdt_base) < 0) {
        printf("WDT node or primary register block not found\n");
        return;
    }

    wdt_disable_auxiliary(node);

#ifndef CHAINLOADING
    if (!is_j713_t8132())
        return;

    wdt_clock_hz = wdt_get_clock_from_adt();
    if (!wdt_clock_hz)
        wdt_clock_hz = J713_WD1_CLOCK_HZ;

    primary_wdt_active = true;
    if (wdt_set_timeout_ms(J713_PRIMARY_TIMEOUT_MS, NULL) < 0) {
        primary_wdt_active = false;
        panic("Failed to establish the J713 primary WD1 lease\n");
    }
#endif
}

bool wdt_primary_is_active(void)
{
    return primary_wdt_active;
}

void wdt_disable(void)
{
    int node;

    if (wdt_get_node_and_primary_from_adt(&node, &wdt_base) < 0) {
        printf("WDT node or primary register block not found\n");
        return;
    }

    printf("Primary WDT register @ 0x%lx\n", wdt_base);
    write32(wdt_base + WDT_CTL, 0);
    wdt_barrier();
    if (read32(wdt_base + WDT_CTL) != 0)
        panic("Failed to disable primary WDT\n");
    primary_wdt_active = false;
    wdt_alarm_ticks = 0;
    printf("Primary WDT disabled\n");

    wdt_disable_auxiliary(node);
}

void wdt_reboot(void)
{
    if (!wdt_base)
        return;

    write32(wdt_base + WDT_ALARM, 0x100000);
    write32(wdt_base + WDT_COUNT, 0);
    write32(wdt_base + WDT_CTL, 4);
}

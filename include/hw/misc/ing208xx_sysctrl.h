/*
 * Ingchips ING208xx system controller / always-on register blocks
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_MISC_ING208XX_SYSCTRL_H
#define HW_MISC_ING208XX_SYSCTRL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define REG32(offset) ((offset) / 4)

/* SYSCTRL page spans up to the GPIOTE block at +0x1f0. */
#define ING208XX_SYSCTRL_NREGS     128

/* One array covers AON2 (0x0000..0x1fff) and AON1 (0x2000..0x3fff). */
#define ING208XX_AON_NREGS         (0x4000 / 4)

#define TYPE_ING208XX_SYSCTRL "ing208xx-sysctrl"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXSysctlState, ING208XX_SYSCTRL)

struct ING208XXSysctlState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[ING208XX_SYSCTRL_NREGS];

    bool slow_clk_rc;
};

#define TYPE_ING208XX_AON "ing208xx-aon"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXAonState, ING208XX_AON)

struct ING208XXAonState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[ING208XX_AON_NREGS];

    qemu_irq slow_clk_sel;
    /*
     * Derived HCLK output: recomputed from the slow-clock select (reg0x10),
     * HCLK select/divider (reg0x18, AON1_REG5) and PLL control (reg0x28)
     * register writes, following the SDK SYSCTRL_GetHClk() formula.
     */
    Clock *hclk;
};

#endif

/*
 * Ingchips ING916xx system controller / always-on register blocks
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_MISC_ING916_SYSCTRL_H
#define HW_MISC_ING916_SYSCTRL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define REG32(offset) ((offset) / 4)

/* The whole 4 KiB APB sub-page belongs to sys_ctrl per the vendor header. */
#define ING916_SYSCTRL_NREGS       (0x1000 / 4)

/*
 * One array covers the AON_APB window head: AON2 (0x0000..0x0fff),
 * RTC (0x1000..0x1fff) and AON1 (0x2000..0x3fff). The tail of the window up
 * to shared RAM stays on an unimplemented-device placeholder in the board.
 */
#define ING916_AON_NREGS           (0x4000 / 4)

#define TYPE_ING916_SYSCTRL "ing916-sysctrl"
OBJECT_DECLARE_SIMPLE_TYPE(ING916SysctrlState, ING916_SYSCTRL)

struct ING916SysctrlState {
    SysBusDevice parent_obj;

    MemoryRegion iomem_sys;
    MemoryRegion iomem_aon;
    uint32_t sys_regs[ING916_SYSCTRL_NREGS];
    uint32_t aon_regs[ING916_AON_NREGS];

    /*
     * Derived HCLK output, recomputed from the SDK SYSCTRL_GetHClk() formula
     * which spans both windows:
     *   slow = AON1 reg0x10 bit8 ? OSC(24 MHz) : internal RC default (64 MHz)
     *   pll  = AON2 reg0x1a8 bit20 ? slow * loop(PllCtrl[14:7]) /
     *          (pre(PllCtrl[6:1]) * out(PllCtrl[20:15])) : 0
     *   hclk = CguCfg[1] bit14 ? pll / div(CguCfg[0][3:0]) : slow
     */
    Clock *hclk;
};

#endif

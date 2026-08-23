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
     * Behavioural clock tree per the vendor RTL block diagram (Renode doc/
     * 916时钟树.md). All outputs are recomputed from the register arrays on
     * every CguCfg/PllCtrl/AON write:
     *
     *   roots:  OSC 24 MHz; internal RC raw 48 MHz fixed /2 -> 24 MHz;
     *           clk_32k 32768 Hz; PLL = ref * loop / (pre * out)
     *   sclk    = slow-root mux (both real sources are 24 MHz)
     *   hclk    = CguCfg[1].bit14 ? pll / div(CguCfg[0][3:0]) : sclk
     *             (the SDK SYSCTRL_GetHClk() formula, verified)
     *   pclk    = hclk / div_pclk_denom
     *   pclk_aon= hclk / div_aon_denom            (112/8 = 14 MHz, cross-checked)
     *   timers  = mux(timer_div_out = sclk/div_timer_denom, clk_32k)
     *   uarts   = mux(sclk, hclk); spi likewise
     *   usb48   = pll_en ? pll / div_usb_denom : 0
     *   adc_out = sclk * num / denom
     *
     * Denominator bitfields marked "(provisional)" below follow the RTL doc
     * defaults but their exact word layout still awaits a 916reg.xlsm sheet
     * cross-check; firmware only reprograms the verified HCLK fields today.
     */
    Clock *sclk;
    Clock *hclk;
    Clock *pclk;
    Clock *pclk_aon;
    Clock *timer_div_out;
    Clock *timer[3];
    Clock *uart[2];
    Clock *spi;
    Clock *usb48;
    Clock *adc_out;
    Clock *clk32k;
};

#endif

/*
 * Ingchips ING916xx system controller and always-on register blocks
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural clock tree following the vendor RTL block diagram and the
 * SDK FAMILY_916 branch of peripheral_sysctrl.c. One device owns the APB
 * sys_ctrl page and the AON pages, so the whole tree recomputes without
 * cross-device signalling:
 *
 *   roots:  OSC 24 MHz; internal RC raw 48 MHz fixed /2 -> 24 MHz;
 *           clk_32k = 32768 Hz; pll = root * loop / (pre * out)
 *   sclk    = slow-root mux (both real roots run at 24 MHz)
 *   hclk    = CguCfg[1] bit14 ? pll / div(CguCfg[0][3:0]) : sclk
 *   pclk    = hclk / div_pclk_denom
 *   pclk_aon= hclk / div_aon_denom              (112/8 = 14 MHz cross-check)
 *   timers  = mux(timer_div_out = sclk / div_timer_denom, clk_32k)
 *   uarts/spi = mux(sclk, hclk)
 *   usb48   = pll_en ? pll / div_usb_denom : 0
 *   adc_out = sclk * num / denom
 *
 * Register reset state models a warm boot after the vendor boot-config
 * clock setup performed by the firmware's config_clk(): OSC 24 MHz ->
 * PLL 336 MHz -> /3 => HCLK 112 MHz, with the timer group left on SCLK
 * (24 MHz, matching TMR_CLK_FREQ == OSC_CLK_FREQ in the SDK).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "hw/core/resettable.h"
#include "hw/misc/ing916_sysctrl.h"

#define ING916_OSC_HZ           (24ull * 1000000)
/* Raw 48 MHz RC divided by a fixed /2 stage (RTL-confirmed). */
#define ING916_RC_HZ            (24ull * 1000000)
#define ING916_CLK32K_HZ        32768ull

/* APB sys_ctrl page at 0x40000000 */
#define ING916_SYS_CGUCFG0      0x00
#define ING916_SYS_CGUCFG1      0x04
#define ING916_SYS_PLLCTRL      0x40

/*
 * CguCfg[0] denominator packing. [3:0] is verified against the SDK
 * SYSCTRL_GetHClk() formula; the remaining fields keep RTL-default values
 * but their exact bit layout awaits a 916reg.xlsm sheet cross-check.
 */
#define ING916_CGU0_DIV_HCLK    0u
#define ING916_CGU0_DIV_PCLK    4u
#define ING916_CGU0_DIV_AON     8u
#define ING916_CGU0_DIV_TMR     12u
#define ING916_CGU0_DIV_USB     16u
#define ING916_CGU0_ADC_NUM     20u
#define ING916_CGU0_ADC_DEN     24u

/* CguCfg[1] source-select bits per the RTL tree (sel_hclk verified). */
#define ING916_CGU1_SEL_TMR(ch) (15u + (ch))
#define ING916_CGU1_SEL_UART(ch) (19u + (ch))
#define ING916_CGU1_SEL_SPI     22u

/* AON window at 0x40100000: AON2 page first, AON1 lives at +0x2000 */
#define ING916_AON_PLL_LOCK     0x01a8
#define ING916_AON_SLOW_SEL     0x2010

static inline uint64_t ing916_field(uint32_t reg, unsigned off)
{
    return (reg >> off) & 0xf;
}

static inline uint64_t ing916_safe_div(uint64_t v)
{
    return v ? v : 1;
}

static void ing916_sysctrl_update_clocks(ING916SysctrlState *s)
{
    uint32_t cgucfg0 = s->sys_regs[REG32(ING916_SYS_CGUCFG0)];
    uint32_t cgucfg1 = s->sys_regs[REG32(ING916_SYS_CGUCFG1)];
    uint32_t pllctrl = s->sys_regs[REG32(ING916_SYS_PLLCTRL)];
    uint32_t slow_sel = s->aon_regs[REG32(ING916_AON_SLOW_SEL)];
    uint32_t pll_lock = s->aon_regs[REG32(ING916_AON_PLL_LOCK)];
    uint64_t root, sclk, pll = 0, hclk;
    unsigned ch;

    root = ((slow_sel >> 8) & 1) ? ING916_OSC_HZ : ING916_RC_HZ;
    sclk = root;

    if ((cgucfg1 >> 14) & 1) {
        if ((pll_lock >> 20) & 1) {
            uint64_t pre = ing916_safe_div((pllctrl >> 1) & 0x3f);
            uint64_t loop = (pllctrl >> 7) & 0xff;
            uint64_t out = ing916_safe_div((pllctrl >> 15) & 0x3f);

            pll = root * loop / (pre * out);
        }
        hclk = pll / ing916_safe_div(ing916_field(cgucfg0,
                                                  ING916_CGU0_DIV_HCLK));
    } else {
        hclk = sclk;
    }

    clock_update_hz(s->hclk, (unsigned)hclk);
    clock_update_hz(s->sclk, (unsigned)sclk);
    clock_update_hz(s->pclk, (unsigned)(hclk / ing916_safe_div(
                        ing916_field(cgucfg0, ING916_CGU0_DIV_PCLK))));
    clock_update_hz(s->pclk_aon, (unsigned)(hclk / ing916_safe_div(
                        ing916_field(cgucfg0, ING916_CGU0_DIV_AON))));

    {
        uint64_t tmr = sclk / ing916_safe_div(ing916_field(cgucfg0,
                                                    ING916_CGU0_DIV_TMR));

        clock_update_hz(s->timer_div_out, (unsigned)tmr);
        for (ch = 0; ch < 3; ch++) {
            bool use_32k = (cgucfg1 >> ING916_CGU1_SEL_TMR(ch)) & 1;

            clock_update_hz(s->timer[ch], (unsigned)(use_32k ?
                                                     ING916_CLK32K_HZ : tmr));
        }
    }

    for (ch = 0; ch < 2; ch++) {
        clock_update_hz(s->uart[ch], (unsigned)(
                ((cgucfg1 >> ING916_CGU1_SEL_UART(ch)) & 1) ? hclk : sclk));
    }
    clock_update_hz(s->spi, (unsigned)(
            ((cgucfg1 >> ING916_CGU1_SEL_SPI) & 1) ? hclk : sclk));

    clock_update_hz(s->usb48, (unsigned)(pll ? pll / ing916_safe_div(
                        ing916_field(cgucfg0, ING916_CGU0_DIV_USB)) : 0));
    clock_update_hz(s->adc_out, (unsigned)(sclk *
            ing916_field(cgucfg0, ING916_CGU0_ADC_NUM) /
            ing916_safe_div(ing916_field(cgucfg0, ING916_CGU0_ADC_DEN))));
    clock_update_hz(s->clk32k, (unsigned)ING916_CLK32K_HZ);
}

static bool ing916_sysctrl_is_hclk_trigger_sys(hwaddr offset)
{
    return offset == ING916_SYS_CGUCFG0 || offset == ING916_SYS_CGUCFG1 ||
           offset == ING916_SYS_PLLCTRL;
}

static bool ing916_sysctrl_is_hclk_trigger_aon(hwaddr offset)
{
    return offset == ING916_AON_SLOW_SEL || offset == ING916_AON_PLL_LOCK;
}

static uint64_t ing916_sysctrl_sys_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    ING916SysctrlState *s = ING916_SYSCTRL(opaque);

    if (offset >= sizeof(s->sys_regs)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read out of range at offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        return 0;
    }
    return s->sys_regs[REG32(offset)];
}

static void ing916_sysctrl_sys_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    ING916SysctrlState *s = ING916_SYSCTRL(opaque);

    if (offset >= sizeof(s->sys_regs)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write out of range at offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        return;
    }
    s->sys_regs[REG32(offset)] = value;

    if (ing916_sysctrl_is_hclk_trigger_sys(offset)) {
        ing916_sysctrl_update_clocks(s);
    }
}

static const MemoryRegionOps ing916_sysctrl_sys_ops = {
    .read = ing916_sysctrl_sys_read,
    .write = ing916_sysctrl_sys_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static uint64_t ing916_sysctrl_aon_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    ING916SysctrlState *s = ING916_SYSCTRL(opaque);

    if (offset >= sizeof(s->aon_regs)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read out of range at offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        return 0;
    }
    return s->aon_regs[REG32(offset)];
}

static void ing916_sysctrl_aon_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    ING916SysctrlState *s = ING916_SYSCTRL(opaque);

    if (offset >= sizeof(s->aon_regs)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write out of range at offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        return;
    }
    s->aon_regs[REG32(offset)] = value;

    if (ing916_sysctrl_is_hclk_trigger_aon(offset)) {
        ing916_sysctrl_update_clocks(s);
    }
}

static const MemoryRegionOps ing916_sysctrl_aon_ops = {
    .read = ing916_sysctrl_aon_read,
    .write = ing916_sysctrl_aon_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * Warm-boot register defaults: OSC 24 MHz selected, PLL locked with
 * pre=1 / loop=14 / out=1 (=336 MHz), HCLK taken from PLL divided by 3,
 * pclk_aon = HCLK/8 (=14 MHz, cross-checked against the RTL tree) and the
 * timer/UART/SPI groups left on SCLK (24 MHz, the SDK TMR_CLK_FREQ).
 */
static void ing916_sysctrl_reset_enter(Object *obj, ResetType type)
{
    ING916SysctrlState *s = ING916_SYSCTRL(obj);

    memset(s->sys_regs, 0, sizeof(s->sys_regs));
    memset(s->aon_regs, 0, sizeof(s->aon_regs));

    s->sys_regs[REG32(ING916_SYS_CGUCFG0)] =
        3 |
        (1 << ING916_CGU0_DIV_PCLK) |
        (8 << ING916_CGU0_DIV_AON) |
        (1 << ING916_CGU0_DIV_TMR) |
        (8 << ING916_CGU0_DIV_USB) |
        (1 << ING916_CGU0_ADC_NUM) |
        (6 << ING916_CGU0_ADC_DEN);
    s->sys_regs[REG32(ING916_SYS_CGUCFG1)] = BIT(14);
    s->sys_regs[REG32(ING916_SYS_PLLCTRL)] =
        (1 << 1) | (14 << 7) | (1 << 15);   /* 24 MHz * 14 / 1 = 336 MHz */

    s->aon_regs[REG32(ING916_AON_SLOW_SEL)] = BIT(8);
    s->aon_regs[REG32(ING916_AON_PLL_LOCK)] = BIT(20);

    ing916_sysctrl_update_clocks(s);
}

static void ing916_sysctrl_init(Object *obj)
{
    ING916SysctrlState *s = ING916_SYSCTRL(obj);
    unsigned ch;

    memory_region_init_io(&s->iomem_sys, obj, &ing916_sysctrl_sys_ops, s,
                          TYPE_ING916_SYSCTRL ".sys", sizeof(s->sys_regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_sys);

    memory_region_init_io(&s->iomem_aon, obj, &ing916_sysctrl_aon_ops, s,
                          TYPE_ING916_SYSCTRL ".aon", sizeof(s->aon_regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_aon);

    s->sclk = qdev_init_clock_out(DEVICE(obj), "sclk");
    s->hclk = qdev_init_clock_out(DEVICE(obj), "hclk");
    s->pclk = qdev_init_clock_out(DEVICE(obj), "pclk");
    s->pclk_aon = qdev_init_clock_out(DEVICE(obj), "pclk_aon");
    s->timer_div_out = qdev_init_clock_out(DEVICE(obj), "timer_div_out");
    for (ch = 0; ch < 3; ch++) {
        static const char *const names[3] = { "timer0", "timer1", "timer2" };

        s->timer[ch] = qdev_init_clock_out(DEVICE(obj), names[ch]);
    }
    for (ch = 0; ch < 2; ch++) {
        static const char *const names[2] = { "uart0", "uart1" };

        s->uart[ch] = qdev_init_clock_out(DEVICE(obj), names[ch]);
    }
    s->spi = qdev_init_clock_out(DEVICE(obj), "spi");
    s->usb48 = qdev_init_clock_out(DEVICE(obj), "usb48");
    s->adc_out = qdev_init_clock_out(DEVICE(obj), "adc_out");
    s->clk32k = qdev_init_clock_out(DEVICE(obj), "clk32k");
}

static const VMStateDescription ing916_sysctrl_vmstate = {
    .name = TYPE_ING916_SYSCTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(sys_regs, ING916SysctrlState,
                             ING916_SYSCTRL_NREGS),
        VMSTATE_UINT32_ARRAY(aon_regs, ING916SysctrlState,
                             ING916_AON_NREGS),
        VMSTATE_END_OF_LIST()
    },
};

static void ing916_sysctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing916_sysctrl_vmstate;
    rc->phases.enter = ing916_sysctrl_reset_enter;
}

static const TypeInfo ing916_sysctrl_info = {
    .name          = TYPE_ING916_SYSCTRL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING916SysctrlState),
    .instance_init = ing916_sysctrl_init,
    .class_init    = ing916_sysctrl_class_init,
};

static void ing916_sysctrl_register_types(void)
{
    type_register_static(&ing916_sysctrl_info);
}

type_init(ing916_sysctrl_register_types)

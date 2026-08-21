/*
 * Ingchips ING208xx system controller (CGU) and always-on registers
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural model: configuration words are stored and returned; no real
 * clock tree is simulated. The one polled hardware behaviour guests rely on
 * is the slow-clock switch status at SYSCTRL offset 0x1c8, whose valid
 * flags must reflect the source selected through AON1 reg0x10 bit 8
 * (SYSCTRL_SelectSlowClk in the SDK).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/misc/ing208xx_sysctrl.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"

#define SYSCTRL_CLK_SWITCH_STATUS   0x1c8
#define AON1_SLOW_CLK_SEL           0x10

static void ing208xx_sysctrl_update_switch_status(ING208XXSysctlState *s)
{
    uint32_t val = s->regs[REG32(SYSCTRL_CLK_SWITCH_STATUS)];

    /* bit2: RC slow clock valid, bit3: RF-24M slow clock valid */
    val &= ~(BIT(2) | BIT(3));
    val |= s->slow_clk_rc ? BIT(2) : BIT(3);
    s->regs[REG32(SYSCTRL_CLK_SWITCH_STATUS)] = val;
}

static uint64_t ing208xx_sysctrl_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    ING208XXSysctlState *s = ING208XX_SYSCTRL(opaque);
    uint32_t val;

    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read out of range at offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        return 0;
    }
    val = s->regs[REG32(offset)];

    if (REG32(offset) == REG32(0x40)) {
        /* pll_ctrl_reg: the PLL locks instantly under emulation */
        val |= BIT(30);
    }
    return val;
}

static void ing208xx_sysctrl_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    ING208XXSysctlState *s = ING208XX_SYSCTRL(opaque);

    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write out of range at offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        return;
    }
    s->regs[REG32(offset)] = value;
}

static const MemoryRegionOps ing208xx_sysctrl_ops = {
    .read = ing208xx_sysctrl_read,
    .write = ing208xx_sysctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ing208xx_sysctrl_realize(DeviceState *dev, Error **errp)
{
    ING208XXSysctlState *s = ING208XX_SYSCTRL(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_sysctrl_ops, s,
                          TYPE_ING208XX_SYSCTRL, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);

    /* Reset: RF-24M slow clock valid (bit3), per the datasheet defaults */
    s->slow_clk_rc = false;
    ing208xx_sysctrl_update_switch_status(s);
}

static void ing208xx_sysctrl_slow_clk_rc(void *opaque, int n, int level)
{
    ING208XXSysctlState *s = ING208XX_SYSCTRL(opaque);

    s->slow_clk_rc = level != 0;
    ing208xx_sysctrl_update_switch_status(s);
}

static void ing208xx_sysctrl_init(Object *obj)
{
    qdev_init_gpio_in(DEVICE(obj), ing208xx_sysctrl_slow_clk_rc, 1);
}

static const VMStateDescription ing208xx_sysctrl_vmstate = {
    .name = TYPE_ING208XX_SYSCTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, ING208XXSysctlState, ING208XX_SYSCTRL_NREGS),
        VMSTATE_BOOL(slow_clk_rc, ING208XXSysctlState),
        VMSTATE_END_OF_LIST()
    },
};

static void ing208xx_sysctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ing208xx_sysctrl_realize;
    dc->vmsd = &ing208xx_sysctrl_vmstate;
}

static const TypeInfo ing208xx_sysctrl_info = {
    .name          = TYPE_ING208XX_SYSCTRL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXSysctlState),
    .instance_init = ing208xx_sysctrl_init,
    .class_init    = ing208xx_sysctrl_class_init,
};

/*
 * Always-on register file covering AON2 (0x40100000) and AON1 (0x40102000).
 * Plain storage except the clock-control words: AON1 reg0x10 (slow-clock
 * source), reg0x18 (AON1_REG5: HCLK select/divider) and reg0x28 (PLL)
 * drive the derived HCLK output following SYSCTRL_GetHClk():
 *   hclk = pll_enabled ? pll / div(REG5[23:20]) : slow_clk
 *   pll  = slow_clk * loop(REG5..[13:7]) / (pre[6:1] * output[20:15])
 */
/* AON1 block lives at +0x2000 inside the region (AON2 takes the first page) */
#define AON_REG_SLOW_SEL    0x2010
#define AON_REG_HCLK        0x2018
#define AON_REG_PLL         0x2028

#define ING208XX_AON_OSC_HZ (24ull * 1000000)

static uint32_t ing208xx_aon_safe_div(uint32_t v)
{
    return v ? v : 1;
}

static void ing208xx_aon_update_hclk(ING208XXAonState *s)
{
    uint32_t slow = ING208XX_AON_OSC_HZ;
    uint64_t hclk;

    if ((s->regs[REG32(AON_REG_SLOW_SEL)] >> 8) & 1) {
        slow = ING208XX_AON_OSC_HZ; /* RF 24M selected */
    }

    if ((s->regs[REG32(AON_REG_HCLK)] >> 30) & 1) {
        uint32_t pll = s->regs[REG32(AON_REG_PLL)];

        if (pll & 1) {
            uint32_t pre = ing208xx_aon_safe_div((pll >> 1) & 0x3f);
            uint32_t loop = (pll >> 7) & 0xff;
            uint32_t out = ing208xx_aon_safe_div((pll >> 15) & 0x3f);
            uint64_t pllfreq = (uint64_t)slow * loop / (pre * out);

            hclk = pllfreq / ing208xx_aon_safe_div(
                            (s->regs[REG32(AON_REG_HCLK)] >> 20) & 0xf);
        } else {
            hclk = 0; /* PLL off: no fast clock available */
        }
    } else {
        hclk = slow;
    }

    clock_update_hz(s->hclk, (unsigned)hclk);
}

static uint64_t ing208xx_aon_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXAonState *s = ING208XX_AON(opaque);

    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read out of range at offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        return 0;
    }
    return s->regs[REG32(offset)];
}

static void ing208xx_aon_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    ING208XXAonState *s = ING208XX_AON(opaque);

    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write out of range at offset 0x%" HWADDR_PRIX "\n",
                      __func__, offset);
        return;
    }
    s->regs[REG32(offset)] = value;

    if (REG32(offset) == REG32(AON1_SLOW_CLK_SEL)) {
        qemu_set_irq(s->slow_clk_sel, (value >> 8) & 1);
    }
    if (REG32(offset) == REG32(AON_REG_SLOW_SEL) ||
        REG32(offset) == REG32(AON_REG_HCLK) ||
        REG32(offset) == REG32(AON_REG_PLL)) {
        ing208xx_aon_update_hclk(s);
    }
}

static const MemoryRegionOps ing208xx_aon_ops = {
    .read = ing208xx_aon_read,
    .write = ing208xx_aon_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ing208xx_aon_init(Object *obj)
{
    ING208XXAonState *s = ING208XX_AON(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->slow_clk_sel, 1);
    s->hclk = qdev_init_clock_out(DEVICE(obj), "hclk");
}

static void ing208xx_aon_realize(DeviceState *dev, Error **errp)
{
    ING208XXAonState *s = ING208XX_AON(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_aon_ops, s,
                          TYPE_ING208XX_AON, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);

    /* Reset: HCLK comes straight from the 24 MHz oscillator */
    clock_update_hz(s->hclk, ING208XX_AON_OSC_HZ);
}

static const VMStateDescription ing208xx_aon_vmstate = {
    .name = TYPE_ING208XX_AON,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, ING208XXAonState, ING208XX_AON_NREGS),
        VMSTATE_END_OF_LIST()
    },
};

static void ing208xx_aon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ing208xx_aon_realize;
    dc->vmsd = &ing208xx_aon_vmstate;
}

static const TypeInfo ing208xx_aon_info = {
    .name          = TYPE_ING208XX_AON,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXAonState),
    .instance_init = ing208xx_aon_init,
    .class_init    = ing208xx_aon_class_init,
};

static void ing208xx_sysctrl_register_types(void)
{
    type_register_static(&ing208xx_sysctrl_info);
    type_register_static(&ing208xx_aon_info);
}

type_init(ing208xx_sysctrl_register_types)

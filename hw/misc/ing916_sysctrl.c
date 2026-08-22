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
 * Behavioural clock tree following the vendor SDK (FAMILY_916 branch of
 * peripheral_sysctrl.c). The HCLK formula spans both windows, so one device
 * owns the APB sys_ctrl page and the AON pages and recomputes its HCLK
 * output without cross-device signalling:
 *
 *   slow = AON1 reg0x10 bit8 ? OSC(24 MHz) : internal RC default (64 MHz;
 *          SLOW_RC_CFG0 is an analog word bring-up never reprograms)
 *   pll  = AON2 reg0x1a8 bit20 ? slow * loop(PllCtrl[14:7]) /
 *          (pre(PllCtrl[6:1]) * out(PllCtrl[20:15])) : 0
 *   hclk = CguCfg[1] bit14 ? pll / div(CguCfg[0][3:0]) : slow
 *
 * Register reset state models a warm boot after the vendor boot-config
 * clock setup performed by the firmware's config_clk(): OSC 24 MHz ->
 * PLL 336 MHz -> /3 => HCLK 112 MHz.
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
#define ING916_RC_DEFAULT_HZ    (64ull * 1000000)

/* APB sys_ctrl page at 0x40000000 */
#define ING916_SYS_CGUCFG0      0x00
#define ING916_SYS_CGUCFG1      0x04
#define ING916_SYS_PLLCTRL      0x40

/* AON window at 0x40100000: AON2 page first, AON1 lives at +0x2000 */
#define ING916_AON_PLL_LOCK     0x01a8
#define ING916_AON_SLOW_SEL     0x2010

static uint32_t ing916_safe_div(uint32_t v)
{
    return v ? v : 1;
}

static void ing916_sysctrl_update_hclk(ING916SysctrlState *s)
{
    uint32_t cgucfg0 = s->sys_regs[REG32(ING916_SYS_CGUCFG0)];
    uint32_t cgucfg1 = s->sys_regs[REG32(ING916_SYS_CGUCFG1)];
    uint32_t pllctrl = s->sys_regs[REG32(ING916_SYS_PLLCTRL)];
    uint32_t slow_sel = s->aon_regs[REG32(ING916_AON_SLOW_SEL)];
    uint32_t pll_lock = s->aon_regs[REG32(ING916_AON_PLL_LOCK)];
    uint32_t slow;
    uint64_t hclk;

    slow = ((slow_sel >> 8) & 1) ? ING916_OSC_HZ : ING916_RC_DEFAULT_HZ;

    if ((cgucfg1 >> 14) & 1) {
        if ((pll_lock >> 20) & 1) {
            uint32_t pre = ing916_safe_div((pllctrl >> 1) & 0x3f);
            uint32_t loop = (pllctrl >> 7) & 0xff;
            uint32_t out = ing916_safe_div((pllctrl >> 15) & 0x3f);
            uint64_t pll = slow * loop / (pre * out);

            hclk = pll / ing916_safe_div(cgucfg0);
        } else {
            /* PLL selected but not running: SYSCTRL_GetPLLClk() returns 0 */
            hclk = 0;
        }
    } else {
        hclk = slow;
    }

    clock_update_hz(s->hclk, (unsigned)hclk);
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
        ing916_sysctrl_update_hclk(s);
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
        ing916_sysctrl_update_hclk(s);
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
 * pre=1 / loop=14 / out=1 (=336 MHz), HCLK taken from PLL divided by 3.
 */
static void ing916_sysctrl_reset_enter(Object *obj, ResetType type)
{
    ING916SysctrlState *s = ING916_SYSCTRL(obj);

    memset(s->sys_regs, 0, sizeof(s->sys_regs));
    memset(s->aon_regs, 0, sizeof(s->aon_regs));

    s->sys_regs[REG32(ING916_SYS_CGUCFG0)] = 3;
    s->sys_regs[REG32(ING916_SYS_CGUCFG1)] = BIT(14);
    s->sys_regs[REG32(ING916_SYS_PLLCTRL)] =
        (1 << 1) | (14 << 7) | (1 << 15);   /* 24 MHz * 14 / 1 = 336 MHz */

    s->aon_regs[REG32(ING916_AON_SLOW_SEL)] = BIT(8);
    s->aon_regs[REG32(ING916_AON_PLL_LOCK)] = BIT(20);

    ing916_sysctrl_update_hclk(s);
}

static void ing916_sysctrl_init(Object *obj)
{
    ING916SysctrlState *s = ING916_SYSCTRL(obj);

    memory_region_init_io(&s->iomem_sys, obj, &ing916_sysctrl_sys_ops, s,
                          TYPE_ING916_SYSCTRL ".sys", sizeof(s->sys_regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_sys);

    memory_region_init_io(&s->iomem_aon, obj, &ing916_sysctrl_aon_ops, s,
                          TYPE_ING916_SYSCTRL ".aon", sizeof(s->aon_regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_aon);

    s->hclk = qdev_init_clock_out(DEVICE(obj), "hclk");
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

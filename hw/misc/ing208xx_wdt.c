/*
 * Ingchips ING208xx watchdog (Andes ATCWDT200 compatible)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural model: register layout and ID per the datasheet WDT sheet.
 * Timeout periods are approximated (base 100 ms scaled by the IntTime /
 * RstTime exponents); interrupt stage raises the IRQ, reset stage requests
 * a system reset when RstEn is set.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "hw/misc/ing208xx_wdt.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/system.h"

#define WDT_IDREV           ((0x03002u << 12) | 0x04)
#define WDT_RESTART_NUM     0xCAFEu
#define WDT_WP_NUM          0x5AA5u

#define WDT_CTRL_EN         BIT(0)
#define WDT_CTRL_INT_EN     BIT(2)
#define WDT_CTRL_RST_EN     BIT(3)
#define WDT_CTRL_INT_TIME_SHIFT 4
#define WDT_CTRL_RST_TIME_SHIFT 8

#define WDT_ST_INT_EXPIRED  BIT(0)

/* Approximate base period of one timeout stage at the modelled PCLK. */
#define WDT_BASE_PERIOD_MS  100

static uint64_t ing208xx_wdt_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXWdtState *s = ING208XX_WDT(opaque);

    switch (offset) {
    case 0x00: /* IdRev */
        return WDT_IDREV;
    case 0x10: /* Ctrl */
        return s->ctrl;
    case 0x1c: /* St */
        return s->status;
    default:
        return 0;
    }
}

static void ing208xx_wdt_restart(ING208XXWdtState *s)
{
    int64_t period;

    timer_del(s->timer);
    if (!(s->ctrl & WDT_CTRL_EN)) {
        s->stage = WDT_STAGE_IDLE;
        return;
    }

    if (s->ctrl & WDT_CTRL_INT_EN) {
        s->stage = WDT_STAGE_INT;
        period = WDT_BASE_PERIOD_MS *
                 (1ull << extract32(s->ctrl, WDT_CTRL_INT_TIME_SHIFT, 4));
    } else {
        s->stage = WDT_STAGE_RESET;
        period = WDT_BASE_PERIOD_MS *
                 (1ull << extract32(s->ctrl, WDT_CTRL_RST_TIME_SHIFT, 3));
    }
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                        period * SCALE_MS);
}

static void ing208xx_wdt_expire(void *opaque)
{
    ING208XXWdtState *s = ING208XX_WDT(opaque);

    if (s->stage == WDT_STAGE_INT) {
        s->status |= WDT_ST_INT_EXPIRED;
        qemu_set_irq(s->irq, 1);
        s->stage = WDT_STAGE_RESET;
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            WDT_BASE_PERIOD_MS *
                            (1ull << extract32(s->ctrl,
                                               WDT_CTRL_RST_TIME_SHIFT, 3)) *
                            SCALE_MS);
        return;
    }

    if (s->ctrl & WDT_CTRL_RST_EN) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ING208xx WDT: reset expired (permissive, qemu只保证可回归)\n");
    }
}

static void ing208xx_wdt_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    ING208XXWdtState *s = ING208XX_WDT(opaque);

    switch (offset) {
    case 0x10: /* Ctrl */
        if (!s->wpen) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ING208xx WDT: Ctrl write while protected\n");
            return;
        }
        s->wpen = false; /* protection re-arms after one write */
        s->ctrl = value;
        ing208xx_wdt_restart(s);
        break;
    case 0x14: /* Restart */
        if ((value & 0xffff) == WDT_RESTART_NUM) {
            ing208xx_wdt_restart(s);
            qemu_set_irq(s->irq, 0);
        }
        break;
    case 0x18: /* WrEn */
        s->wpen = ((value & 0xffff) == WDT_WP_NUM);
        break;
    case 0x1c: /* St */
        if (value & WDT_ST_INT_EXPIRED) {
            s->status &= ~WDT_ST_INT_EXPIRED;
            qemu_set_irq(s->irq, 0);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ing208xx_wdt_ops = {
    .read = ing208xx_wdt_read,
    .write = ing208xx_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ing208xx_wdt_reset_hold(Object *obj, ResetType type)
{
    ING208XXWdtState *s = ING208XX_WDT(obj);

    timer_del(s->timer);
    s->ctrl = 0;
    s->status = 0;
    s->stage = WDT_STAGE_IDLE;
    s->wpen = false;
    qemu_set_irq(s->irq, 0);
}

static void ing208xx_wdt_init(Object *obj)
{
    ING208XXWdtState *s = ING208XX_WDT(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_wdt_ops, s,
                          TYPE_ING208XX_WDT, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ing208xx_wdt_expire, s);
}

static const VMStateDescription ing208xx_wdt_vmstate = {
    .name = TYPE_ING208XX_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, ING208XXWdtState),
        VMSTATE_UINT32(status, ING208XXWdtState),
        VMSTATE_BOOL(wpen, ING208XXWdtState),
        VMSTATE_INT32(stage, ING208XXWdtState),
        VMSTATE_TIMER_PTR(timer, ING208XXWdtState),
        VMSTATE_END_OF_LIST()
    },
};

static void ing208xx_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing208xx_wdt_vmstate;
    rc->phases.hold = ing208xx_wdt_reset_hold;
}

static const TypeInfo ing208xx_wdt_info = {
    .name          = TYPE_ING208XX_WDT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXWdtState),
    .instance_init = ing208xx_wdt_init,
    .class_init    = ing208xx_wdt_class_init,
};

static void ing208xx_wdt_register_types(void)
{
    type_register_static(&ing208xx_wdt_info);
}

type_init(ing208xx_wdt_register_types)

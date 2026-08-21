/*
 * Ingchips ING208xx GPIO (Andes ATCGPIO100 compatible)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural model: output values are echoed back on DataIn for pins
 * configured as outputs (internal loopback); input pins read the level of
 * the corresponding external gpio line (floating low when unwired).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/gpio/ing208xx_gpio.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/bitops.h"

#define GPIO_IDREV           ((0x020310u << 8) | 0x02)

static uint32_t ing208xx_gpio_data_in(ING208XXGpioState *s)
{
    /* Outputs loop back internally; inputs sample the external lines. */
    return (s->data_out & s->dir) |
           (s->ext_level & ~s->dir & 0xffffffffu);
}

static void ing208xx_gpio_update_irq(ING208XXGpioState *s)
{
    qemu_set_irq(s->irq, s->intr_status & s->intr_en);
}

static uint64_t ing208xx_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXGpioState *s = ING208XX_GPIO(opaque);

    switch (offset) {
    case 0x00:
        return GPIO_IDREV;
    case 0x10:
        return s->cfg;
    case 0x20:
        return ing208xx_gpio_data_in(s);
    case 0x24:
        return s->data_out;
    case 0x28:
        return s->dir;
    case 0x34:
        return s->ioie;
    case 0x50:
        return s->intr_en;
    case 0x54:
    case 0x58:
    case 0x5c:
        return s->intr_mode[(offset - 0x54) / 4];
    case 0x64:
        return s->intr_status;
    case 0x70:
        return s->debounce_en;
    case 0x74:
        return s->debounce_ctrl;
    default:
        return 0;
    }
}

static void ing208xx_gpio_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    ING208XXGpioState *s = ING208XX_GPIO(opaque);

    switch (offset) {
    case 0x10:
        s->cfg = value;
        break;
    case 0x24:
        s->data_out = value;
        break;
    case 0x28:
        s->dir = value;
        break;
    case 0x2c: /* DoutClear */
        s->data_out &= ~value;
        break;
    case 0x30: /* DoutSet */
        s->data_out |= value;
        break;
    case 0x34:
        s->ioie = value;
        break;
    case 0x50:
        s->intr_en = value;
        ing208xx_gpio_update_irq(s);
        break;
    case 0x54:
    case 0x58:
    case 0x5c:
        s->intr_mode[(offset - 0x54) / 4] = value;
        break;
    case 0x64: /* IntrStatus, w1c */
        s->intr_status &= ~value;
        ing208xx_gpio_update_irq(s);
        break;
    case 0x70:
        s->debounce_en = value;
        break;
    case 0x74:
        s->debounce_ctrl = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ing208xx_gpio_ops = {
    .read = ing208xx_gpio_read,
    .write = ing208xx_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ing208xx_gpio_set_line(void *opaque, int n, int level)
{
    ING208XXGpioState *s = ING208XX_GPIO(opaque);

    if (level) {
        s->ext_level |= BIT(n);
    } else {
        s->ext_level &= ~BIT(n);
    }
}

static void ing208xx_gpio_reset_hold(Object *obj, ResetType type)
{
    ING208XXGpioState *s = ING208XX_GPIO(obj);

    s->cfg = 0;
    s->data_out = 0;
    s->dir = 0;
    s->ioie = 0;
    s->intr_en = 0;
    memset(s->intr_mode, 0, sizeof(s->intr_mode));
    s->intr_status = 0;
    s->debounce_en = 0;
    s->debounce_ctrl = 0;
    qemu_set_irq(s->irq, 0);
}

static void ing208xx_gpio_init(Object *obj)
{
    ING208XXGpioState *s = ING208XX_GPIO(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_gpio_ops, s,
                          TYPE_ING208XX_GPIO, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);

    qdev_init_gpio_in(dev, ing208xx_gpio_set_line, ING208XX_GPIO_PINS);
}

static const VMStateDescription ing208xx_gpio_vmstate = {
    .name = TYPE_ING208XX_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cfg, ING208XXGpioState),
        VMSTATE_UINT32(data_out, ING208XXGpioState),
        VMSTATE_UINT32(dir, ING208XXGpioState),
        VMSTATE_UINT32(ioie, ING208XXGpioState),
        VMSTATE_UINT32_ARRAY(intr_mode, ING208XXGpioState, 3),
        VMSTATE_UINT32(intr_status, ING208XXGpioState),
        VMSTATE_UINT32(debounce_en, ING208XXGpioState),
        VMSTATE_UINT32(debounce_ctrl, ING208XXGpioState),
        VMSTATE_END_OF_LIST()
    },
};

static void ing208xx_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing208xx_gpio_vmstate;
    rc->phases.hold = ing208xx_gpio_reset_hold;
}

static const TypeInfo ing208xx_gpio_info = {
    .name          = TYPE_ING208XX_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXGpioState),
    .instance_init = ing208xx_gpio_init,
    .class_init    = ing208xx_gpio_class_init,
};

static void ing208xx_gpio_register_types(void)
{
    type_register_static(&ing208xx_gpio_info);
}

type_init(ing208xx_gpio_register_types)

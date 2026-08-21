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

static uint32_t ing208xx_gpio_levels(ING208XXGpioState *s)
{
    /* outputs loop back internally; inputs sample the external lines */
    return (s->data_out & s->dir) |
           (s->ext_level & ~s->dir & 0xffffffffu);
}

/*
 * Interrupt trigger modes per the datasheet IntrMode field:
 *   0 none, 2 high level, 3 low level,
 *   5 falling edge, 6 rising edge, 7 both edges.
 * IntrStatus latches until cleared (w1c).
 */
#define GPIO_MODE_HIGH      0x2
#define GPIO_MODE_LOW       0x3
#define GPIO_MODE_FALL      0x5
#define GPIO_MODE_RISE      0x6
#define GPIO_MODE_BOTH      0x7

static void ing208xx_gpio_update_int(ING208XXGpioState *s)
{
    uint32_t cur = ing208xx_gpio_levels(s);
    uint32_t prev = s->prev_input;
    uint32_t pending = 0;
    int pin;

    for (pin = 0; pin < ING208XX_GPIO_PINS; pin++) {
        uint32_t bit = BIT(pin);
        uint32_t mode;

        if (!((s->intr_en >> pin) & 1)) {
            continue;
        }
        mode = (s->intr_mode[pin / 8] >> ((pin % 8) * 4)) & 0x7;
        switch (mode) {
        case GPIO_MODE_HIGH:
            if (cur & bit) {
                pending |= bit;
            }
            break;
        case GPIO_MODE_LOW:
            if (!(cur & bit)) {
                pending |= bit;
            }
            break;
        case GPIO_MODE_FALL:
            if ((prev & bit) && !(cur & bit)) {
                pending |= bit;
            }
            break;
        case GPIO_MODE_RISE:
            if (!(prev & bit) && (cur & bit)) {
                pending |= bit;
            }
            break;
        case GPIO_MODE_BOTH:
            if ((prev & bit) != (cur & bit)) {
                pending |= bit;
            }
            break;
        default:
            break;
        }
    }

    s->intr_status |= pending;
    s->prev_input = cur;
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
        return ing208xx_gpio_levels(s);
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
        ing208xx_gpio_update_int(s);
        break;
    case 0x28:
        s->dir = value;
        ing208xx_gpio_update_int(s);
        break;
    case 0x2c: /* DoutClear */
        s->data_out &= ~value;
        ing208xx_gpio_update_int(s);
        break;
    case 0x30: /* DoutSet */
        s->data_out |= value;
        ing208xx_gpio_update_int(s);
        break;
    case 0x34:
        s->ioie = value;
        ing208xx_gpio_update_int(s);
        break;
    case 0x50:
        s->intr_en = value;
        ing208xx_gpio_update_int(s);
        break;
    case 0x54:
    case 0x58:
    case 0x5c:
        s->intr_mode[(offset - 0x54) / 4] = value;
        ing208xx_gpio_update_int(s);
        break;
    case 0x64: /* IntrStatus, w1c */
        s->intr_status &= ~value;
        ing208xx_gpio_update_int(s);
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
    ing208xx_gpio_update_int(s);
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
    /* board pull-ups: unconnected inputs idle high */
    s->ext_level = 0xffffffffu;
    s->prev_input = ing208xx_gpio_levels(s);
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

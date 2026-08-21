/*
 * Ingchips ING208xx I2C controller
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural model: configuration registers are stored and returned.
 * Bus transactions (attached slave devices) are not emulated yet.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/i2c/ing208xx_i2c.h"
#include "hw/core/sysbus.h"

#define REG32(offset) ((offset) / 4)

static uint64_t ing208xx_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXI2cState *s = ING208XX_I2C(opaque);

    if (offset < sizeof(s->regs)) {
        return s->regs[REG32(offset)];
    }
    return 0;
}

static void ing208xx_i2c_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    ING208XXI2cState *s = ING208XX_I2C(opaque);

    if (offset < sizeof(s->regs)) {
        s->regs[REG32(offset)] = value;
    }
}

static const MemoryRegionOps ing208xx_i2c_ops = {
    .read = ing208xx_i2c_read,
    .write = ing208xx_i2c_write,
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

static void ing208xx_i2c_reset_hold(Object *obj, ResetType type)
{
    ING208XXI2cState *s = ING208XX_I2C(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void ing208xx_i2c_init(Object *obj)
{
    ING208XXI2cState *s = ING208XX_I2C(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_i2c_ops, s,
                          TYPE_ING208XX_I2C, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void ing208xx_i2c_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ing208xx_i2c_reset_hold;
}

static const TypeInfo ing208xx_i2c_info = {
    .name          = TYPE_ING208XX_I2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXI2cState),
    .instance_init = ing208xx_i2c_init,
    .class_init    = ing208xx_i2c_class_init,
};

static void ing208xx_i2c_register_types(void)
{
    type_register_static(&ing208xx_i2c_info);
}

type_init(ing208xx_i2c_register_types)

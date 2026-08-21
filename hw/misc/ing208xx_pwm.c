/*
 * Ingchips ING208xx PWM controller
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural model: configuration registers are stored and returned.
 * Waveform generation is not emulated yet.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/misc/ing208xx_pwm.h"
#include "hw/core/sysbus.h"

#define REG32(offset) ((offset) / 4)

static uint64_t ing208xx_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXPwmState *s = ING208XX_PWM(opaque);

    if (offset < sizeof(s->regs)) {
        return s->regs[REG32(offset)];
    }
    return 0;
}

static void ing208xx_pwm_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    ING208XXPwmState *s = ING208XX_PWM(opaque);

    if (offset < sizeof(s->regs)) {
        s->regs[REG32(offset)] = value;
    }
}

static const MemoryRegionOps ing208xx_pwm_ops = {
    .read = ing208xx_pwm_read,
    .write = ing208xx_pwm_write,
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

static void ing208xx_pwm_reset_hold(Object *obj, ResetType type)
{
    ING208XXPwmState *s = ING208XX_PWM(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void ing208xx_pwm_init(Object *obj)
{
    ING208XXPwmState *s = ING208XX_PWM(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_pwm_ops, s,
                          TYPE_ING208XX_PWM, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void ing208xx_pwm_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ing208xx_pwm_reset_hold;
}

static const TypeInfo ing208xx_pwm_info = {
    .name          = TYPE_ING208XX_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXPwmState),
    .instance_init = ing208xx_pwm_init,
    .class_init    = ing208xx_pwm_class_init,
};

static void ing208xx_pwm_register_types(void)
{
    type_register_static(&ing208xx_pwm_info);
}

type_init(ing208xx_pwm_register_types)

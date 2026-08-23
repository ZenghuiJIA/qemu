/*
 * Ingchips ING208xx SAR-ADC and true-random-number-generator
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural models: the SARADC is a register store returning a fixed
 * conversion value for enabled channels; the TRNG returns fresh
 * pseudo-random data on every data-register read.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/misc/ing208xx_sadc.h"
#include "qemu/timer.h"
#include "qemu/guest-random.h"

/* ---- SARADC ---- */

static uint32_t ing208xx_sadc_data(ING208XXSadcState *s);

#define SADC_REG_CFG0       0x00
#define SADC_REG_DATA       0x0c
#define SADC_REG_STATUS     0x10
#define SADC_REG_CHAN_DATA  0x20

static void ing208xx_sadc_complete(void *opaque);

static void ing208xx_sadc_complete(void *opaque)
{
    ING208XXSadcState *s = ING208XX_SADC(opaque);
    s->converting = false;
    s->regs[REG32(SADC_REG_STATUS)] = 1;
}

static uint64_t ing208xx_sadc_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXSadcState *s = ING208XX_SADC(opaque);

    switch (offset) {
    case SADC_REG_DATA:
        return ing208xx_sadc_data(s);
    case SADC_REG_STATUS:
        return s->converting ? 0 : 1;
    default:
        if (offset >= SADC_REG_CHAN_DATA &&
            offset < SADC_REG_CHAN_DATA + 8 * 4) {
            int ch = (offset - SADC_REG_CHAN_DATA) / 4;
            return s->channel_value[ch];
        }
        if (offset < sizeof(s->regs)) {
            return s->regs[offset / 4];
        }
        return 0;
    }
}

static void ing208xx_sadc_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    ING208XXSadcState *s = ING208XX_SADC(opaque);

    if (offset >= SADC_REG_CHAN_DATA &&
        offset < SADC_REG_CHAN_DATA + 8 * 4) {
        int ch = (offset - SADC_REG_CHAN_DATA) / 4;
        s->channel_value[ch] = (uint16_t)value;
        s->fixed_value = (uint16_t)value;
        return;
    }
    if (offset < sizeof(s->regs) && offset != SADC_REG_DATA) {
        uint32_t old = s->regs[offset / 4];
        s->regs[offset / 4] = (uint32_t)value;
        if (offset == 0x04 && value != 0 && old == 0 && !s->converting) {
            int n = __builtin_popcount((uint32_t)value);
            s->converting = true;
            s->regs[REG32(SADC_REG_STATUS)] = 0;
            timer_mod(s->conv_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          10000 * (n ? n : 1));
        }
        if (offset == SADC_REG_CFG0 && (value & 1) && !(old & 1) &&
            !s->converting) {
            s->converting = true;
            s->regs[REG32(SADC_REG_STATUS)] = 0;
            timer_mod(s->conv_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 50000);
        }
    }
}

static const MemoryRegionOps ing208xx_sadc_ops = {
    .read = ing208xx_sadc_read,
    .write = ing208xx_sadc_write,
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

static uint32_t ing208xx_sadc_data(ING208XXSadcState *s)
{
    uint32_t ch_en = s->regs[REG32(0x04)]; /* enabled channels */
    int pin = ch_en ? ctz32(ch_en | 0x80000000u) : 0;
    uint16_t val = s->channel_value[pin] ? s->channel_value[pin]
                                         : s->fixed_value;

    return ((uint32_t)val << 4) | (uint32_t)pin;
}

static void ing208xx_sadc_reset_hold(Object *obj, ResetType type)
{
    ING208XXSadcState *s = ING208XX_SADC(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->channel_value, 0, sizeof(s->channel_value));
    s->fixed_value = 0x800;
    s->converting = false;
    timer_del(s->conv_timer);
    s->regs[REG32(SADC_REG_STATUS)] = 1;
}

static void ing208xx_sadc_init(Object *obj)
{
    ING208XXSadcState *s = ING208XX_SADC(obj);

    s->conv_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ing208xx_sadc_complete, s);
    for (int i = 0; i < 8; i++) {
        s->channel_value[i] = 0x800 + i * 0x100;
    }
    s->fixed_value = 0x800;
    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_sadc_ops, s,
                          TYPE_ING208XX_SADC, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription ing208xx_sadc_vmstate = {
    .name = TYPE_ING208XX_SADC,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, ING208XXSadcState, ING208XX_SADC_NREGS),
        VMSTATE_UINT16(fixed_value, ING208XXSadcState),
        VMSTATE_UINT16_ARRAY(channel_value, ING208XXSadcState, 8),
        VMSTATE_BOOL(converting, ING208XXSadcState),
        VMSTATE_TIMER_PTR(conv_timer, ING208XXSadcState),
        VMSTATE_END_OF_LIST()
    },
};

static void ing208xx_sadc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing208xx_sadc_vmstate;
    rc->phases.hold = ing208xx_sadc_reset_hold;
}

static const TypeInfo ing208xx_sadc_info = {
    .name          = TYPE_ING208XX_SADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXSadcState),
    .instance_init = ing208xx_sadc_init,
    .class_init    = ing208xx_sadc_class_init,
};

/* ---- TRNG ---- */

static uint64_t ing208xx_trng_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXTrngState *s = ING208XX_TRNG(opaque);
    uint32_t val = 0;

    switch (offset) {
    case 0x10: /* TRNG_DATA */
        qemu_guest_getrandom_nofail(&val, sizeof(val));
        return val;
    case 0x14: /* TRNG_STATUS: valid */
        return BIT(0);
    default:
        if (offset < sizeof(s->regs)) {
            return s->regs[offset / 4];
        }
        return 0;
    }
}

static void ing208xx_trng_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    ING208XXTrngState *s = ING208XX_TRNG(opaque);

    if (offset < sizeof(s->regs)) {
        s->regs[offset / 4] = value;
    }
}

static const MemoryRegionOps ing208xx_trng_ops = {
    .read = ing208xx_trng_read,
    .write = ing208xx_trng_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ing208xx_trng_init(Object *obj)
{
    ING208XXTrngState *s = ING208XX_TRNG(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_trng_ops, s,
                          TYPE_ING208XX_TRNG, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const TypeInfo ing208xx_trng_info = {
    .name          = TYPE_ING208XX_TRNG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXTrngState),
    .instance_init = ing208xx_trng_init,
};

static void ing208xx_periph_register_types(void)
{
    type_register_static(&ing208xx_sadc_info);
    type_register_static(&ing208xx_trng_info);
}

type_init(ing208xx_periph_register_types)

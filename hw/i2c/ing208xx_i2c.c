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

    switch (offset) {
    case 0x18: /* Status: CMPL done; bit0 reflects RX delivery queue */
    {
        uint32_t st = s->regs[REG32(offset)] | (1u << 9);

        if (s->rx_cnt > 0) {
            st &= ~(1u << 0);
        } else {
            st |= (1u << 0);
        }
        return st;
    }
    case 0x20: /* Data: pop prepared RX byte */
    {
        uint8_t v = 0xff;

        if (s->rx_cnt > 0) {
            v = s->rxq[s->rx_head];
            s->rx_head++;
            s->rx_cnt--;
        }
        return v;
    }
    default:
        if (offset < sizeof(s->regs)) {
            return s->regs[REG32(offset)];
        }
        return 0;
    }
}

/*
 * Issue a data transaction against the virtual slave:
 *  - queued TX bytes: first byte is the slave register pointer, the rest
 *    are values written at consecutive addresses
 *  - otherwise prepare an RX window starting at the current pointer
 */
static void ing208xx_i2c_issue(ING208XXI2cState *s)
{
    int i;

    if (s->txq_len > 0) {
        s->sensor_ptr = s->txq[0];
        for (i = 1; i < s->txq_len; i++) {
            s->sensor_mem[(s->sensor_ptr + i - 1) & 0xff] = s->txq[i];
        }
        s->txq_len = 0;
    }

    s->rx_head = 0;
    s->rx_cnt = 8;
    for (i = 0; i < 8; i++) {
        s->rxq[i] = s->sensor_mem[(s->sensor_ptr + i) & 0xff];
    }
}

static void ing208xx_i2c_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    ING208XXI2cState *s = ING208XX_I2C(opaque);

    switch (offset) {
    case 0x20: /* Data write: queue TX byte */
        if (s->txq_len < 256) {
            s->txq[s->txq_len++] = value & 0xff;
        }
        break;
    case 0x28: /* Cmd: ISSUE_DATA_TRANSACTION runs the transfer */
        if ((value & 0xff) == 1) {
            ing208xx_i2c_issue(s);
        }
        break;
    default:
        if (offset < sizeof(s->regs)) {
            s->regs[REG32(offset)] = value;
        }
        break;
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
    memset(s->sensor_mem, 0, sizeof(s->sensor_mem));
    s->sensor_mem[0] = 0x87; /* STK8BA58 chip id */
    s->sensor_ptr = 0;
    memset(s->txq, 0, sizeof(s->txq));
    memset(s->rxq, 0, sizeof(s->rxq));
    s->txq_len = 0;
    s->rx_head = 0;
    s->rx_cnt = 0;
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

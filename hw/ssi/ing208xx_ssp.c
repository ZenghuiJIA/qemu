/*
 * Ingchips ING208xx SPI controller (SSP, QSPI and APB instances)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural model: configuration registers are stored; transmit data
 * loops back into the receive FIFO when a transfer is started through
 * the Cmd register, so FIFO flag polling never stalls. Attached SPI
 * devices are not emulated yet.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/ssi/ing208xx_ssp.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/log.h"

#define REG_CTRL        0x30
#define REG_CMD         0x24
#define REG_DATA        0x2c
#define REG_STATUS      0x34
#define REG_INTRST      0x3c

#define ST_RXEMPTY      BIT(14)
#define ST_RXFULL       BIT(15)
#define ST_TXEMPTY      BIT(22)
#define ST_TXFULL       BIT(23)

#define INTRST_END      BIT(4)

#define FIFO_DEPTH      16

static void ing208xx_ssp_update_status(ING208XXSspState *s)
{
    uint32_t st = s->regs[REG32(REG_STATUS)];

    st &= ~(ST_RXEMPTY | ST_RXFULL | ST_TXEMPTY | ST_TXFULL |
            0x3f << 8 | 0x3f << 16);
    if (s->rx_len == 0) {
        st |= ST_RXEMPTY;
    }
    if (s->rx_len == FIFO_DEPTH) {
        st |= ST_RXFULL;
    }
    st |= (uint32_t)s->rx_len << 8;
    if (s->tx_len == 0) {
        st |= ST_TXEMPTY;
    }
    if (s->tx_len == FIFO_DEPTH) {
        st |= ST_TXFULL;
    }
    st |= (uint32_t)s->tx_len << 16;
    s->regs[REG32(REG_STATUS)] = st;
}

static void ing208xx_ssp_update_irq(ING208XXSspState *s)
{
    qemu_set_irq(s->irq, s->regs[REG32(REG_INTRST)] &
                         s->regs[REG32(0x38)] ? 1 : 0);
}

static uint64_t ing208xx_ssp_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXSspState *s = ING208XX_SSP(opaque);

    switch (offset) {
    case REG_DATA: {
        uint8_t v = 0xff;

        if (s->rx_len > 0) {
            v = s->rx_fifo[s->rx_head];
            s->rx_head = (s->rx_head + 1) % FIFO_DEPTH;
            s->rx_len--;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: read from empty RX FIFO\n", __func__);
        }
        ing208xx_ssp_update_status(s);
        return v;
    }
    default:
        if (offset < sizeof(s->regs)) {
            return s->regs[offset / 4];
        }
        return 0;
    }
}

static void ing208xx_ssp_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    ING208XXSspState *s = ING208XX_SSP(opaque);

    switch (offset) {
    case REG_CTRL:
        /*
         * Ctrl bits 0..2 are self-clearing software resets that never
         * read back as set: SPIRST flushes both FIFOs, RXFIFORST and
         * TXFIFORST flush their own FIFO.
         */
        if (value & BIT(0)) {
            s->tx_head = s->tx_len = 0;
            s->rx_head = s->rx_len = 0;
        }
        if (value & BIT(1)) {
            s->rx_head = s->rx_len = 0;
        }
        if (value & BIT(2)) {
            s->tx_head = s->tx_len = 0;
        }
        value &= ~(BIT(0) | BIT(1) | BIT(2));
        s->regs[REG32(REG_CTRL)] = value;
        break;
    case REG_DATA:
        if (s->tx_len < FIFO_DEPTH) {
            s->tx_fifo[(s->tx_head + s->tx_len) % FIFO_DEPTH] = value;
            s->tx_len++;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: write to full TX FIFO\n", __func__);
        }
        break;
    case REG_CMD:
        /* full-duplex loopback: queued TX bytes land in the RX FIFO */
        while (s->tx_len > 0 && s->rx_len < FIFO_DEPTH) {
            s->rx_fifo[(s->rx_head + s->rx_len) % FIFO_DEPTH] =
                s->tx_fifo[s->tx_head];
            s->tx_head = (s->tx_head + 1) % FIFO_DEPTH;
            s->tx_len--;
            s->rx_len++;
        }
        s->regs[REG32(REG_INTRST)] |= INTRST_END;
        ing208xx_ssp_update_irq(s);
        break;
    case REG_INTRST: /* w1c */
        s->regs[REG32(REG_INTRST)] &= ~value;
        ing208xx_ssp_update_irq(s);
        break;
    default:
        if (offset < sizeof(s->regs)) {
            s->regs[offset / 4] = value;
        }
        break;
    }
    ing208xx_ssp_update_status(s);
}

static const MemoryRegionOps ing208xx_ssp_ops = {
    .read = ing208xx_ssp_read,
    .write = ing208xx_ssp_write,
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

static void ing208xx_ssp_reset_hold(Object *obj, ResetType type)
{
    ING208XXSspState *s = ING208XX_SSP(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->tx_head = s->tx_len = 0;
    s->rx_head = s->rx_len = 0;
    ing208xx_ssp_update_status(s);
    qemu_set_irq(s->irq, 0);
}

static void ing208xx_ssp_init(Object *obj)
{
    ING208XXSspState *s = ING208XX_SSP(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_ssp_ops, s,
                          TYPE_ING208XX_SSP, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static const VMStateDescription ing208xx_ssp_vmstate = {
    .name = TYPE_ING208XX_SSP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, ING208XXSspState, ING208XX_SSP_NREGS),
        VMSTATE_UINT8_ARRAY(tx_fifo, ING208XXSspState, FIFO_DEPTH),
        VMSTATE_UINT8_ARRAY(rx_fifo, ING208XXSspState, FIFO_DEPTH),
        VMSTATE_INT32(tx_head, ING208XXSspState),
        VMSTATE_INT32(tx_len, ING208XXSspState),
        VMSTATE_INT32(rx_head, ING208XXSspState),
        VMSTATE_INT32(rx_len, ING208XXSspState),
        VMSTATE_END_OF_LIST()
    },
};

static void ing208xx_ssp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing208xx_ssp_vmstate;
    rc->phases.hold = ing208xx_ssp_reset_hold;
}

static const TypeInfo ing208xx_ssp_info = {
    .name          = TYPE_ING208XX_SSP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXSspState),
    .instance_init = ing208xx_ssp_init,
    .class_init    = ing208xx_ssp_class_init,
};

static void ing208xx_ssp_register_types(void)
{
    type_register_static(&ing208xx_ssp_info);
}

type_init(ing208xx_ssp_register_types)

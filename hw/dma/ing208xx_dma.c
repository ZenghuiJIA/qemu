/*
 * Ingchips ING208xx descriptor-based DMA controller
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural model: memory-to-memory transfers execute synchronously at
 * channel-enable time and raise the transfer-complete flag. Peripheral
 * handshakes (request lines encoded as pseudo source addresses) are stored
 * but not serviced yet - guests using UART RX rely on the PL011 receive
 * timeout path instead.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/dma/ing208xx_dma.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "qemu/log.h"

/* Descriptor layout inside a channel block (stride 0x20) */
#define CH_REG_CTRL         0x0
#define CH_REG_TRANSIZE     0x1
#define CH_REG_SRCADDR      0x2
#define CH_REG_DSTADDR      0x4

#define DESC_CTRL_ENABLE    BIT(0)
/* Width fields are SET for 32-bit transfers, CLEAR for byte transfers */
#define DESC_CTRL_DST_WORD  BIT(19)
#define DESC_CTRL_SRC_WORD  BIT(22)

/* Global registers */
#define DMA_REG_CTRL        0x20
#define DMA_REG_ABORT       0x24
#define DMA_REG_DMACTRL0    0x28
#define DMA_REG_DMACTRL1    0x2c
#define DMA_REG_INTSTATUS   0x30

static inline uint32_t dma_ch_reg(int ch, int reg)
{
    return 0x40 + ch * 0x20 + reg * 4;
}

/*
 * Interrupt status encoding used by the SDK decoder: per-channel error
 * at bit ch, abort at bit 8+ch, transfer complete at bit 16+ch.
 */
#define DMA_INT_ERR(ch)     BIT(ch)
#define DMA_INT_ABORT(ch)   BIT(8 + (ch))
#define DMA_INT_TC(ch)      BIT(16 + (ch))
static void ing208xx_dma_update_irq(ING208XXDmaState *s)
{
    qemu_set_irq(s->irq, s->int_status != 0);
}

static bool ing208xx_dma_is_mem(hwaddr addr)
{
    return (addr >= 0x20000000u && addr < 0x2000C000u) ||
           (addr >= 0x40120000u && addr < 0x4012A000u);
}

static void ing208xx_dma_start_channel(ING208XXDmaState *s, int ch)
{
    uint32_t ctrl = s->regs[REG32(dma_ch_reg(ch, CH_REG_CTRL))];
    uint32_t size = s->regs[REG32(dma_ch_reg(ch, CH_REG_TRANSIZE))];
    hwaddr src = s->regs[REG32(dma_ch_reg(ch, CH_REG_SRCADDR))];
    hwaddr dst = s->regs[REG32(dma_ch_reg(ch, CH_REG_DSTADDR))];
    uint32_t unit = (ctrl & (DESC_CTRL_DST_WORD | DESC_CTRL_SRC_WORD)) ? 4 : 1;
    AddressSpace *as = &address_space_memory;
    int i;
    bool src_periph = !ing208xx_dma_is_mem(src);
    bool dst_periph = !ing208xx_dma_is_mem(dst);

    if (!(ctrl & DESC_CTRL_ENABLE)) {
        return;
    }

    s->fifo_level[ch] = 0;
    for (i = 0; i < (int)size; i++) {
        uint8_t buf[4];
        hwaddr soff = src + i * unit;
        hwaddr doff = dst + i * unit;

        if (src_periph) {
            buf[0] = 0xa5 + (uint8_t)i;
            if (unit > 1) {
                memset(buf + 1, 0xa5, unit - 1);
            }
        } else {
            address_space_read(as, soff, MEMTXATTRS_UNSPECIFIED, buf, unit);
        }

        if (dst_periph) {
            s->fifo_level[ch] = (s->fifo_level[ch] + 1) % 16;
        } else {
            if (!ing208xx_dma_is_mem(doff)) {
                s->int_status |= DMA_INT_ERR(ch);
                break;
            }
            address_space_write(as, doff, MEMTXATTRS_UNSPECIFIED, buf, unit);
            s->fifo_level[ch] = (s->fifo_level[ch] + 1) % 16;
        }
        if (s->fifo_level[ch] == 0 && i + 1 < (int)size) {
            continue;
        }
    }
    s->int_status |= DMA_INT_TC(ch);
    ing208xx_dma_update_irq(s);
}

static uint64_t ing208xx_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXDmaState *s = ING208XX_DMA(opaque);

    switch (offset) {
    case DMA_REG_CTRL:
        return s->ctrl;
    case DMA_REG_DMACTRL0:
        return s->dmactrl[0];
    case DMA_REG_DMACTRL1:
        return s->dmactrl[1];
    case DMA_REG_INTSTATUS:
        return s->int_status;
    default:
        if (offset >= 0x40 && offset < sizeof(s->regs)) {
            return s->regs[REG32(offset)];
        }
        return 0;
    }
}

static void ing208xx_dma_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    ING208XXDmaState *s = ING208XX_DMA(opaque);
    int ch;

    switch (offset) {
    case DMA_REG_CTRL:
        s->ctrl = value;
        if (value & 1) {
            memset(&s->regs[REG32(0x40)], 0, ING208XX_DMA_CHANNELS * 0x20);
        }
        return;
    case DMA_REG_DMACTRL0:
        s->dmactrl[0] = value;
        return;
    case DMA_REG_DMACTRL1:
        s->dmactrl[1] = value;
        return;
    case DMA_REG_ABORT:
        for (ch = 0; ch < ING208XX_DMA_CHANNELS; ch++) {
            if (value & BIT(ch)) {
                uint32_t idx = REG32(dma_ch_reg(ch, CH_REG_CTRL));

                s->regs[idx] &= ~DESC_CTRL_ENABLE;
                s->int_status |= DMA_INT_ABORT(ch);
            }
        }
        ing208xx_dma_update_irq(s);
        return;
    case DMA_REG_INTSTATUS: /* w1c */
        s->int_status &= ~value;
        ing208xx_dma_update_irq(s);
        return;
    default:
        break;
    }

    if (offset >= 0x40 && offset < sizeof(s->regs)) {
        s->regs[REG32(offset)] = value;
        for (ch = 0; ch < ING208XX_DMA_CHANNELS; ch++) {
            if (offset == dma_ch_reg(ch, CH_REG_CTRL)) {
                ing208xx_dma_start_channel(s, ch);
            }
        }
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: write to unknown offset 0x%" HWADDR_PRIX "\n",
                  __func__, offset);
}

static const MemoryRegionOps ing208xx_dma_ops = {
    .read = ing208xx_dma_read,
    .write = ing208xx_dma_write,
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

static void ing208xx_dma_reset_hold(Object *obj, ResetType type)
{
    ING208XXDmaState *s = ING208XX_DMA(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->ctrl = 0;
    s->int_status = 0;
    s->dmactrl[0] = 0x76543210u;
    s->dmactrl[1] = 0x76543210u;
    memset(s->fifo_level, 0, sizeof(s->fifo_level));
    qemu_set_irq(s->irq, 0);
}

static void ing208xx_dma_init(Object *obj)
{
    ING208XXDmaState *s = ING208XX_DMA(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_dma_ops, s,
                          TYPE_ING208XX_DMA, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static const VMStateDescription ing208xx_dma_vmstate = {
    .name = TYPE_ING208XX_DMA,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, ING208XXDmaState, ING208XX_DMA_NREGS),
        VMSTATE_UINT32(ctrl, ING208XXDmaState),
        VMSTATE_UINT32(int_status, ING208XXDmaState),
        VMSTATE_UINT32_ARRAY(dmactrl, ING208XXDmaState, 2),
        VMSTATE_UINT32_ARRAY(fifo_level, ING208XXDmaState,
                             ING208XX_DMA_CHANNELS),
        VMSTATE_END_OF_LIST()
    },
};

static void ing208xx_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing208xx_dma_vmstate;
    rc->phases.hold = ing208xx_dma_reset_hold;
}

static const TypeInfo ing208xx_dma_info = {
    .name          = TYPE_ING208XX_DMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXDmaState),
    .instance_init = ing208xx_dma_init,
    .class_init    = ing208xx_dma_class_init,
};

static void ing208xx_dma_register_types(void)
{
    type_register_static(&ing208xx_dma_info);
}

type_init(ing208xx_dma_register_types)

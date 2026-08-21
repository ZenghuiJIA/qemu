/*
 * Ingchips ING208xx descriptor-based DMA controller
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_DMA_ING208XX_DMA_H
#define HW_DMA_ING208XX_DMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define REG32(offset) ((offset) / 4)

#define ING208XX_DMA_CHANNELS      8
/* channel blocks span 0x40 .. 0x13c; allocate up to the end of the page */
#define ING208XX_DMA_NREGS         (0x200 / 4)

#define TYPE_ING208XX_DMA "ing208xx-dma"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXDmaState, ING208XX_DMA)

struct ING208XXDmaState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ING208XX_DMA_NREGS];
    uint32_t ctrl;
    uint32_t int_status;
};

#endif

/*
 * Ingchips ING208xx SPI controller (SSP)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_SSI_ING208XX_SSP_H
#define HW_SSI_ING208XX_SSP_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define REG32(offset) ((offset) / 4)

#define ING208XX_SSP_NREGS         (0x100 / 4)

#define TYPE_ING208XX_SSP "ing208xx-ssp"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXSspState, ING208XX_SSP)

struct ING208XXSspState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    SSIBus *ssi;

    uint32_t regs[ING208XX_SSP_NREGS];
    uint8_t tx_fifo[16];
    uint8_t rx_fifo[16];
    int32_t tx_head;
    int32_t tx_len;
    int32_t rx_head;
    int32_t rx_len;
    bool is_qspi;
};

#endif

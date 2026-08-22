/*
 * Ingchips ING208xx I2C controller
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_I2C_ING208XX_I2C_H
#define HW_I2C_ING208XX_I2C_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#ifndef REG32
#define REG32(offset) ((offset) / 4)
#endif

#define ING208XX_I2C_NREGS         (0x100 / 4)

#define TYPE_ING208XX_I2C "ing208xx-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXI2cState, ING208XX_I2C)

struct ING208XXI2cState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[ING208XX_I2C_NREGS];

    /* virtual STK8BA58 slave: per-register echo, reg0 = chip id 0x87 */
    uint8_t sensor_mem[256];
    uint8_t sensor_ptr;

    /* master transaction engine state */
    uint8_t txq[256];
    int32_t txq_len;
    uint8_t rxq[16];
    int32_t rx_head;
    int32_t rx_cnt;
};

#endif

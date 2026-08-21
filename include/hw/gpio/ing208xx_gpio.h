/*
 * Ingchips ING208xx GPIO (Andes ATCGPIO100 compatible)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_GPIO_ING208XX_GPIO_H
#define HW_GPIO_ING208XX_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define ING208XX_GPIO_PINS        32

#define TYPE_ING208XX_GPIO "ing208xx-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXGpioState, ING208XX_GPIO)

struct ING208XXGpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t ext_level;

    uint32_t cfg;
    uint32_t data_out;
    uint32_t dir;
    uint32_t ioie;
    uint32_t intr_en;
    uint32_t intr_mode[3];
    uint32_t intr_status;
    uint32_t debounce_en;
    uint32_t debounce_ctrl;

    uint32_t prev_input; /* last sampled levels, for edge detection */
};

#endif

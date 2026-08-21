/*
 * Ingchips ING208xx SAR-ADC and true-random-number-generator
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_MISC_ING208XX_SADC_H
#define HW_MISC_ING208XX_SADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define REG32(offset) ((offset) / 4)

#define ING208XX_SADC_NREGS        (0x100 / 4)
#define ING208XX_TRNG_NREGS        (0x100 / 4)

#define TYPE_ING208XX_SADC "ing208xx-sadc"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXSadcState, ING208XX_SADC)

struct ING208XXSadcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[ING208XX_SADC_NREGS];
    uint16_t fixed_value; /* conversion result returned for enabled channels */
};

#define TYPE_ING208XX_TRNG "ing208xx-trng"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXTrngState, ING208XX_TRNG)

struct ING208XXTrngState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[ING208XX_TRNG_NREGS];
};

#endif

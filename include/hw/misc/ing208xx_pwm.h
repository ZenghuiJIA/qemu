/*
 * Ingchips ING208xx PWM controller
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_MISC_ING208XX_PWM_H
#define HW_MISC_ING208XX_PWM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#ifndef REG32
#define REG32(offset) ((offset) / 4)
#endif

#define ING208XX_PWM_NREGS         (0x200 / 4)

#define TYPE_ING208XX_PWM "ing208xx-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXPwmState, ING208XX_PWM)

struct ING208XXPwmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[ING208XX_PWM_NREGS];
};

#endif

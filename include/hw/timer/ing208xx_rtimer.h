/*
 * Ingchips ING208xx reduced-function timer (RTMR)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_TIMER_ING208XX_RTIMER_H
#define HW_TIMER_ING208XX_RTIMER_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_ING208XX_RTIMER "ing208xx-rtimer"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXRtimerState, ING208XX_RTIMER)

struct ING208XXRtimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    uint32_t cnt;
    uint32_t cmp;
    uint32_t ctl;
    bool running;
    uint64_t acc_ns;
    uint64_t start_ns;
};

#endif

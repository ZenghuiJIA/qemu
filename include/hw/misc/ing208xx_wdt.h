/*
 * Ingchips ING208xx watchdog (Andes ATCWDT200 compatible)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_MISC_ING208XX_WDT_H
#define HW_MISC_ING208XX_WDT_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

typedef enum WdtStage {
    WDT_STAGE_IDLE = 0,
    WDT_STAGE_INT,
    WDT_STAGE_RESET,
} WdtStage;

#define TYPE_ING208XX_WDT "ing208xx-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXWdtState, ING208XX_WDT)

struct ING208XXWdtState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    uint32_t ctrl;
    uint32_t status;
    bool wpen;
    int stage;
};

#endif

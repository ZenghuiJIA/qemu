/*
 * Ingchips ING208xx periodic interval timer (PIT, two channels)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_TIMER_ING208XX_PIT_H
#define HW_TIMER_ING208XX_PIT_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_ING208XX_PIT "ing208xx-pit"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXPitState, ING208XX_PIT)

typedef struct Ing208xxPitChannel {
    QEMUTimer *timer;
    struct ING208XXPitState *pit;
    int idx;

    uint32_t ctrl;
    uint32_t reload;
    bool running;
    uint64_t acc_ns;
    uint64_t start_ns;
    qemu_irq irq;
} Ing208xxPitChannel;

struct ING208XXPitState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    Ing208xxPitChannel ch[2];

    uint32_t int_en;
    uint32_t int_st;
    uint32_t ch_en;
};

uint32_t ing208xx_pit_get_counter(ING208XXPitState *s, int ch);

#endif

/*
 * Ingchips ING208xx reduced-function timer (RTMR)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural model: CNT is a free-running up-counter clocked from a fixed
 * 32 kHz tick; on reaching CMP in wrapping mode it wraps to zero and latches
 * CTL.IS. Firmware clears IS by writing 1 (RTMR_IntClr).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/timer/ing208xx_rtimer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/bitops.h"

/* RTMR runs from its dedicated 128 MHz clock domain (vendor default). */
#define RTMR_TICK_HZ        128000000ull

#define RTMR_CTL_EN         BIT(0)
#define RTMR_CTL_CC         BIT(1)  /* write 1: clear counter */
#define RTMR_CTL_OM_SHIFT   2
#define RTMR_CTL_IE         BIT(4)
#define RTMR_CTL_IS         BIT(6)

static uint64_t ing208xx_rtimer_count(ING208XXRtimerState *s)
{
    uint64_t elapsed_ns;

    if (!s->running) {
        return s->cnt;
    }

    elapsed_ns = s->acc_ns +
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->start_ns;
    return (s->cnt + muldiv64(elapsed_ns, RTMR_TICK_HZ,
                              NANOSECONDS_PER_SECOND)) & 0xffffffffull;
}

static bool ing208xx_rtimer_count_crossed(ING208XXRtimerState *s)
{
    return s->cmp != 0 && ing208xx_rtimer_count(s) >= s->cmp;
}

/*
 * Schedule the next compare-match event. Free-run mode never matches;
 * wrapping mode re-arms from the tick handler after each hit.
 */
static void ing208xx_rtimer_arm_match(ING208XXRtimerState *s)
{
    uint32_t cnt = ing208xx_rtimer_count(s);
    uint32_t om = extract32(s->ctl, RTMR_CTL_OM_SHIFT, 2);
    uint64_t delta;
    int64_t now;

    if (om == 2 || s->cmp == 0 || !s->running) {
        return;
    }

    if (cnt >= s->cmp) {
        delta = 0;
    } else {
        delta = muldiv64((uint64_t)(s->cmp - cnt), NANOSECONDS_PER_SECOND,
                         RTMR_TICK_HZ);
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(s->timer, now + delta);
}

static void ing208xx_rtimer_checkpoint(ING208XXRtimerState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->running) {
        s->cnt = ing208xx_rtimer_count(s);
        s->acc_ns = 0;
    }
    s->start_ns = now;
}

static void ing208xx_rtimer_update_irq(ING208XXRtimerState *s)
{
    qemu_set_irq(s->irq, (s->ctl & RTMR_CTL_IE) && (s->ctl & RTMR_CTL_IS));
}

static void ing208xx_rtimer_reschedule(ING208XXRtimerState *s)
{
    bool running = s->ctl & RTMR_CTL_EN;

    ing208xx_rtimer_checkpoint(s);
    timer_del(s->timer);
    s->running = running;

    if (!running) {
        qemu_set_irq(s->irq, 0);
        return;
    }

    ing208xx_rtimer_arm_match(s);
}

static void ing208xx_rtimer_tick(void *opaque)
{
    ING208XXRtimerState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t om = extract32(s->ctl, RTMR_CTL_OM_SHIFT, 2);

    if (!ing208xx_rtimer_count_crossed(s)) {
        return;
    }

    s->ctl |= RTMR_CTL_IS;
    ing208xx_rtimer_update_irq(s);

    if (om == 1) {
        /* one-shot: reset at compare and stop */
        s->cnt = 0;
        s->acc_ns = 0;
        s->start_ns = now;
        s->ctl &= ~RTMR_CTL_EN;
        s->running = false;
        timer_del(s->timer);
        return;
    }

    /* continuous wrapping: restart from zero and re-arm */
    s->cnt = 0;
    s->acc_ns = 0;
    s->start_ns = now;
    ing208xx_rtimer_arm_match(s);
}

static uint64_t ing208xx_rtimer_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    ING208XXRtimerState *s = ING208XX_RTIMER(opaque);

    switch (offset) {
    case 0x00: /* TMR_CNT */
        return ing208xx_rtimer_count(s);
    case 0x04: /* TMR_CMP */
        return s->cmp;
    case 0x08: /* TMR_CTL */
        return s->ctl & ~RTMR_CTL_CC;
    default:
        return 0;
    }
}

static void ing208xx_rtimer_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    ING208XXRtimerState *s = ING208XX_RTIMER(opaque);

    switch (offset) {
    case 0x04:
        s->cmp = value;
        break;
    case 0x08:
        if (value & RTMR_CTL_CC) {
            ing208xx_rtimer_checkpoint(s);
            s->cnt = 0;
        }
        /*
         * CTL.IS is write-1-to-clear (RTMR_IntClr passes back the whole
         * register with IS set); every other field is plain rw.
         */
        s->ctl = value & ~RTMR_CTL_CC;
        s->ctl &= ~(value & RTMR_CTL_IS);
        ing208xx_rtimer_update_irq(s);
        ing208xx_rtimer_reschedule(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ing208xx_rtimer_ops = {
    .read = ing208xx_rtimer_read,
    .write = ing208xx_rtimer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ing208xx_rtimer_reset_hold(Object *obj, ResetType type)
{
    ING208XXRtimerState *s = ING208XX_RTIMER(obj);

    timer_del(s->timer);
    s->cnt = 0;
    s->cmp = 0;
    s->ctl = 0;
    s->running = false;
    s->acc_ns = 0;
    s->start_ns = 0;
    qemu_set_irq(s->irq, 0);
}

static void ing208xx_rtimer_init(Object *obj)
{
    ING208XXRtimerState *s = ING208XX_RTIMER(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &ing208xx_rtimer_ops, s,
                          TYPE_ING208XX_RTIMER, 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ing208xx_rtimer_tick, s);
}

static const VMStateDescription ing208xx_rtimer_vmstate = {
    .name = TYPE_ING208XX_RTIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cnt, ING208XXRtimerState),
        VMSTATE_UINT32(cmp, ING208XXRtimerState),
        VMSTATE_UINT32(ctl, ING208XXRtimerState),
        VMSTATE_BOOL(running, ING208XXRtimerState),
        VMSTATE_UINT64(acc_ns, ING208XXRtimerState),
        VMSTATE_UINT64(start_ns, ING208XXRtimerState),
        VMSTATE_TIMER_PTR(timer, ING208XXRtimerState),
        VMSTATE_END_OF_LIST()
    },
};

static void ing208xx_rtimer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing208xx_rtimer_vmstate;
    rc->phases.hold = ing208xx_rtimer_reset_hold;
}

static const TypeInfo ing208xx_rtimer_info = {
    .name          = TYPE_ING208XX_RTIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXRtimerState),
    .instance_init = ing208xx_rtimer_init,
    .class_init    = ing208xx_rtimer_class_init,
};

static void ing208xx_rtimer_register_types(void)
{
    type_register_static(&ing208xx_rtimer_info);
}

type_init(ing208xx_rtimer_register_types)

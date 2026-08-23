/*
 * Ingchips ING208xx periodic interval timer (PIT, two channels)
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Behavioural model of the datasheet TIMER block (ID 0x03031): each of the
 * two channels provides a 32-bit down-counter (ChMode 1) clocked from the
 * APB clock; on underflow the channel reloads and raises its interrupt
 * status bit. Other channel modes are accepted but only mode 1 counts.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/timer/ing208xx_pit.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "qemu/bitops.h"

#define PIT_IDREV           ((0x03031u << 12) | 0x03)
#define PIT_NUM_CHANNELS    2

/* Legacy fixed PCLK assumption for boards without a modelled clock tree. */
#define PIT_APB_HZ          (128ull * 1000000)

static inline uint64_t pit_clk_hz(const ING208XXPitState *s)
{
    uint64_t hz = clock_get_hz(s->pclk);

    return hz ? hz : PIT_APB_HZ;
}

enum PitReg {
    PIT_REG_CFG      = 0x10,
    PIT_REG_INT_EN   = 0x14,
    PIT_REG_INT_ST   = 0x18,
    PIT_REG_CH_EN    = 0x1c,
};

static inline hwaddr pit_ch_reg(int ch, hwaddr base)
{
    return base + ch * 0x10;
}

static inline int64_t pit_period_ns(const ING208XXPitState *s,
                                    uint32_t reload)
{
    if (reload == 0) {
        return 0;
    }
    return muldiv64((uint64_t)reload + 1, NANOSECONDS_PER_SECOND,
                    pit_clk_hz(s));
}

static bool ing208xx_pit_ch_running(ING208XXPitState *s, int ch)
{
    Ing208xxPitChannel *c = &s->ch[ch];
    uint32_t mode = extract32(c->ctrl, 0, 3);
    uint32_t enables = (s->ch_en >> (ch * 4)) & 0xf;

    /*
     * ChEn packs four sub-timer enables per channel (bit offset ch*4);
     * the channel runs when any sub-timer relevant to the current split
     * mode is enabled.
     */
    switch (mode) {
    case 1:
        return (enables & 0x1) != 0;
    case 2:
        return (enables & 0x3) != 0;
    case 3:
        return (enables & 0xf) != 0;
    default:
        return false;
    }
}

static void ing208xx_pit_reschedule(ING208XXPitState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ch;

    for (ch = 0; ch < PIT_NUM_CHANNELS; ch++) {
        Ing208xxPitChannel *c = &s->ch[ch];
        bool running = ing208xx_pit_ch_running(s, ch);
        uint32_t mode = extract32(c->ctrl, 0, 3);
        uint32_t period_reload;

        if (mode == 2) {
            period_reload = c->reload & 0xffff;
        } else if (mode == 3) {
            period_reload = c->reload & 0xff;
        } else {
            period_reload = c->reload;
        }

        /*
         * Fold time since the last checkpoint into the accumulator, but
         * only while the channel has been counting; a fresh start begins
         * a new accounting period.
         */
        if (c->running) {
            c->acc_ns += now - c->start_ns;
        }
        if (running && !c->running) {
            c->acc_ns = 0;
        }
        c->start_ns = now;

        timer_del(c->timer);
        c->running = running;
        if (running) {
            timer_mod(c->timer, now + pit_period_ns(s, period_reload));
        } else {
            qemu_set_irq(c->irq, 0);
        }
    }
}

uint32_t ing208xx_pit_get_counter(ING208XXPitState *s, int ch)
{
    Ing208xxPitChannel *c = &s->ch[ch];
    uint64_t elapsed_ns, ticks;
    uint32_t mode, shift, nsubs, n;

    if (!c->running) {
        return c->reload;
    }

    elapsed_ns = c->acc_ns +
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - c->start_ns;
    ticks = muldiv64(elapsed_ns, pit_clk_hz(s), NANOSECONDS_PER_SECOND);

    mode = extract32(c->ctrl, 0, 3);
    if (mode == 1) {
        return c->reload - (uint32_t)(ticks % (c->reload + 1));
    }

    if (mode == 2) {
        shift = 16;
        nsubs = 2;
    } else {
        shift = 8;
        nsubs = 4;
    }

    /*
     * Split modes: the reload word divides into independent down-counters,
     * each gated by its own ChEn enable bit and holding its reload value
     * while disabled.
     */
    {
        uint32_t mask = (1u << shift) - 1;
        uint32_t result = 0;

        for (n = 0; n < nsubs; n++) {
            uint32_t sub_reload = (c->reload >> (n * shift)) & mask;
            bool sub_en = (s->ch_en >> (ch * 4 + n)) & 1;
            uint32_t val = sub_reload;

            if (sub_en) {
                val = sub_reload - (uint32_t)(ticks % (sub_reload + 1));
            }
            result |= val << (n * shift);
        }
        return result;
    }
}

static void ing208xx_pit_update_irq(ING208XXPitState *s)
{
    int ch;

    for (ch = 0; ch < PIT_NUM_CHANNELS; ch++) {
        Ing208xxPitChannel *c = &s->ch[ch];
        int bit = ch * 4;

        qemu_set_irq(c->irq,
                     ((s->int_en >> bit) & 1) && ((s->int_st >> bit) & 1));
    }
}

static void ing208xx_pit_tick(void *opaque)
{
    Ing208xxPitChannel *c = opaque;
    ING208XXPitState *s = c->pit;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int bit = c->idx * 4;

    c->acc_ns = 0;
    c->start_ns = now;
    s->int_st |= BIT(bit);
    ing208xx_pit_update_irq(s);

    if (ing208xx_pit_ch_running(s, c->idx)) {
        timer_mod(c->timer, now + pit_period_ns(s, c->reload));
    }
}

static uint64_t ing208xx_pit_read(void *opaque, hwaddr offset, unsigned size)
{
    ING208XXPitState *s = ING208XX_PIT(opaque);
    int ch;

    switch (offset) {
    case 0x00:
        return PIT_IDREV;
    case PIT_REG_CFG:
        return PIT_NUM_CHANNELS;
    case PIT_REG_INT_EN:
        return s->int_en;
    case PIT_REG_INT_ST:
        return s->int_st;
    case PIT_REG_CH_EN:
        return s->ch_en;
    default:
        break;
    }

    for (ch = 0; ch < PIT_NUM_CHANNELS; ch++) {
        if (offset == pit_ch_reg(ch, 0x20)) {
            return s->ch[ch].ctrl;
        }
        if (offset == pit_ch_reg(ch, 0x24)) {
            return s->ch[ch].reload;
        }
        if (offset == pit_ch_reg(ch, 0x28)) {
            return ing208xx_pit_get_counter(s, ch);
        }
    }
    return 0;
}

static void ing208xx_pit_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    ING208XXPitState *s = ING208XX_PIT(opaque);
    int ch;

    switch (offset) {
    case PIT_REG_INT_EN:
        s->int_en = value & 0xff;
        ing208xx_pit_update_irq(s);
        return;
    case PIT_REG_INT_ST: /* w1c */
        s->int_st &= ~(value & 0xff);
        ing208xx_pit_update_irq(s);
        return;
    case PIT_REG_CH_EN:
        s->ch_en = value & 0xff;
        ing208xx_pit_reschedule(s);
        return;
    default:
        break;
    }

    for (ch = 0; ch < PIT_NUM_CHANNELS; ch++) {
        if (offset == pit_ch_reg(ch, 0x20)) {
            s->ch[ch].ctrl = value & 0x1f;
            ing208xx_pit_reschedule(s);
            return;
        }
        if (offset == pit_ch_reg(ch, 0x24)) {
            s->ch[ch].reload = value;
            ing208xx_pit_reschedule(s);
            return;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: write to unknown offset 0x%" HWADDR_PRIX "\n",
                  __func__, offset);
}

static const MemoryRegionOps ing208xx_pit_ops = {
    .read = ing208xx_pit_read,
    .write = ing208xx_pit_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ing208xx_pit_reset_hold(Object *obj, ResetType type)
{
    ING208XXPitState *s = ING208XX_PIT(obj);
    int ch;

    s->int_en = 0;
    s->int_st = 0;
    s->ch_en = 0;
    for (ch = 0; ch < PIT_NUM_CHANNELS; ch++) {
        Ing208xxPitChannel *c = &s->ch[ch];

        timer_del(c->timer);
        c->ctrl = 0;
        c->reload = 0;
        c->running = false;
        c->acc_ns = 0;
        c->start_ns = 0;
        qemu_set_irq(c->irq, 0);
    }
}

static void ing208xx_pit_init(Object *obj)
{
    ING208XXPitState *s = ING208XX_PIT(obj);
    int ch;

    memory_region_init_io(&s->iomem, OBJECT(obj), &ing208xx_pit_ops, s,
                          TYPE_ING208XX_PIT, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);

    s->pclk = qdev_init_clock_in(DEVICE(obj), "pclk", NULL, NULL, 0);

    for (ch = 0; ch < PIT_NUM_CHANNELS; ch++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->ch[ch].irq);
        s->ch[ch].pit = s;
        s->ch[ch].idx = ch;
        s->ch[ch].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       ing208xx_pit_tick, &s->ch[ch]);
    }
}

static const VMStateDescription ing208xx_pit_channel_vmstate = {
    .name = "ing208xx-pit-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, Ing208xxPitChannel),
        VMSTATE_UINT32(reload, Ing208xxPitChannel),
        VMSTATE_BOOL(running, Ing208xxPitChannel),
        VMSTATE_UINT64(acc_ns, Ing208xxPitChannel),
        VMSTATE_UINT64(start_ns, Ing208xxPitChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription ing208xx_pit_vmstate = {
    .name = TYPE_ING208XX_PIT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(int_en, ING208XXPitState),
        VMSTATE_UINT32(int_st, ING208XXPitState),
        VMSTATE_UINT32(ch_en, ING208XXPitState),
        VMSTATE_STRUCT_ARRAY(ch, ING208XXPitState, PIT_NUM_CHANNELS, 1,
                             ing208xx_pit_channel_vmstate,
                             Ing208xxPitChannel),
        VMSTATE_END_OF_LIST()
    },
};

static void ing208xx_pit_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing208xx_pit_vmstate;
    rc->phases.hold = ing208xx_pit_reset_hold;
}

static const TypeInfo ing208xx_pit_info = {
    .name          = TYPE_ING208XX_PIT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXPitState),
    .instance_init = ing208xx_pit_init,
    .class_init    = ing208xx_pit_class_init,
};

static void ing208xx_pit_register_types(void)
{
    type_register_static(&ing208xx_pit_info);
}

type_init(ing208xx_pit_register_types)

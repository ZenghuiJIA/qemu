/*
 * ING916 PWM - 4 channel timer/PWM block at 0x40005000
 *
 * Forked layout from the ING208xx PWM: four independent channels with
 * ChCtrl/Reload/Cntr triplets and a shared IntEn/IntSt/ChEn.  Each
 * channel ticks against pclk (or clk32k when ChClk=1).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/irq.h"
#include "qom/object.h"

#define TYPE_ING916_PWM "ing916-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(Ing916PwmState, ING916_PWM)

#define PWM_NCH 4

struct Ing916PwmChannelCtx {
    Ing916PwmState *s;
    int ch;
};

struct Ing916PwmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irqs[4];
    Clock *clks[4];
    QEMUTimer *timers[4];
    struct Ing916PwmChannelCtx ctx[4];
    uint32_t regs[0x100 / 4];
    uint32_t ch_reload[PWM_NCH];
    uint32_t ch_cntr[PWM_NCH];
    uint32_t ch_ctrl[PWM_NCH];
};

static void ing916_pwm_update_irq(Ing916PwmState *s, int ch)
{
    uint32_t inten = s->regs[0x14 / 4];
    uint32_t intst = s->regs[0x18 / 4];
    bool en = (inten >> (ch * 4)) & 0xf;
    bool st = (intst >> (ch * 4)) & 0xf;
    qemu_set_irq(s->irqs[ch], en && st);
}

static void ing916_pwm_tick(void *opaque)
{
    struct Ing916PwmChannelCtx *ctx = opaque;
    Ing916PwmState *s = ctx->s;
    int ch = ctx->ch;

    if (s->ch_cntr[ch] == 0) {
        s->ch_cntr[ch] = s->ch_reload[ch] ? s->ch_reload[ch] : 1;
        s->regs[0x18 / 4] |= 1u << (ch * 4);
        ing916_pwm_update_irq(s, ch);
    } else {
        s->ch_cntr[ch]--;
        if (s->ch_cntr[ch] == 0) {
            s->regs[0x18 / 4] |= 1u << (ch * 4);
            ing916_pwm_update_irq(s, ch);
            s->ch_reload[ch] = s->regs[(0x24 + ch * 0x10) / 4];
            s->ch_cntr[ch] = s->ch_reload[ch] ? s->ch_reload[ch] : 1;
        }
    }
    uint64_t hz = s->clks[ch] ? clock_get_hz(s->clks[ch]) : 0;
    if (!hz) {
        hz = 24000000;
    }
    timer_mod(s->timers[ch],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000 / hz);
}

static uint64_t ing916_pwm_read(void *opaque, hwaddr off, unsigned size)
{
    Ing916PwmState *s = ING916_PWM(opaque);

    switch (off) {
    case 0x00: return 0x03031003u;
    case 0x10: return 0x00000002u;
    case 0x24: case 0x34: case 0x44: case 0x54:
        return s->ch_reload[(off - 0x24) / 0x10];
    case 0x28: case 0x38: case 0x48: case 0x58:
        return s->ch_cntr[(off - 0x28) / 0x10];
    default:
        if (off < sizeof(s->regs)) {
            return s->regs[off >> 2];
        }
        return 0;
    }
}

static void ing916_pwm_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    Ing916PwmState *s = ING916_PWM(opaque);

    if (off == 0x18) {
        s->regs[off >> 2] &= ~((uint32_t)val & 0xffff);
        for (int i = 0; i < PWM_NCH; i++) {
            ing916_pwm_update_irq(s, i);
        }
        return;
    }

    if (off == 0x1c) {
        uint32_t old = s->regs[off >> 2];
        s->regs[off >> 2] = (uint32_t)val;
        for (int i = 0; i < PWM_NCH; i++) {
            bool was = (old >> i) & 1;
            bool now = (val >> i) & 1;
            if (now && !was) {
                s->ch_cntr[i] = s->ch_reload[i] ? s->ch_reload[i] : 1;
                uint64_t hz = s->clks[i] ? clock_get_hz(s->clks[i]) : 0;
                if (!hz) {
                    hz = 24000000;
                }
                timer_mod(s->timers[i],
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000 / hz);
            } else if (!now && was) {
                timer_del(s->timers[i]);
            }
        }
        return;
    }

    if (off == 0x20 || off == 0x30 || off == 0x40 || off == 0x50) {
        int ch = (off - 0x20) / 0x10;
        s->ch_ctrl[ch] = (uint32_t)val;
        s->regs[off >> 2] = (uint32_t)val;
        return;
    }
    if (off == 0x24 || off == 0x34 || off == 0x44 || off == 0x54) {
        int ch = (off - 0x24) / 0x10;
        s->ch_reload[ch] = (uint32_t)val;
        s->regs[off >> 2] = (uint32_t)val;
        return;
    }

    if (off < sizeof(s->regs)) {
        s->regs[off >> 2] = (uint32_t)val;
        if (off == 0x14) {
            for (int i = 0; i < PWM_NCH; i++) {
                ing916_pwm_update_irq(s, i);
            }
        }
    }
}

static const MemoryRegionOps ing916_pwm_ops = {
    .read = ing916_pwm_read,
    .write = ing916_pwm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ing916_pwm_reset_enter(Object *obj, ResetType type)
{
    Ing916PwmState *s = ING916_PWM(obj);

    memset(s->regs, 0, sizeof(s->regs));
    for (int i = 0; i < PWM_NCH; i++) {
        s->ch_reload[i] = 0;
        s->ch_cntr[i] = 0;
        s->ch_ctrl[i] = 0;
        timer_del(s->timers[i]);
        qemu_set_irq(s->irqs[i], 0);
    }
}

static void ing916_pwm_init(Object *obj)
{
    Ing916PwmState *s = ING916_PWM(obj);

    memory_region_init_io(&s->iomem, obj, &ing916_pwm_ops, s,
                          TYPE_ING916_PWM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    for (int i = 0; i < PWM_NCH; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irqs[i]);
        s->ctx[i].s = s;
        s->ctx[i].ch = i;
        s->timers[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL, ing916_pwm_tick,
                                    &s->ctx[i]);
    }
    for (int i = 0; i < PWM_NCH; i++) {
        s->clks[i] = NULL;
    }
}

static const VMStateDescription ing916_pwm_vmstate = {
    .name = TYPE_ING916_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Ing916PwmState, 0x100 / 4),
        VMSTATE_UINT32_ARRAY(ch_reload, Ing916PwmState, PWM_NCH),
        VMSTATE_UINT32_ARRAY(ch_cntr, Ing916PwmState, PWM_NCH),
        VMSTATE_UINT32_ARRAY(ch_ctrl, Ing916PwmState, PWM_NCH),
        VMSTATE_END_OF_LIST()
    }
};

static void ing916_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing916_pwm_vmstate;
    rc->phases.enter = ing916_pwm_reset_enter;
}

static const TypeInfo ing916_pwm_info = {
    .name          = TYPE_ING916_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Ing916PwmState),
    .instance_init = ing916_pwm_init,
    .class_init    = ing916_pwm_class_init,
};

static void ing916_pwm_register_types(void)
{
    type_register_static(&ing916_pwm_info);
}

type_init(ing916_pwm_register_types)

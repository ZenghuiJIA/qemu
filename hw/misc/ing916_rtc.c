/*
 * ING916 RTC - 32k counter with alarm interrupt (n02)
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

#define TYPE_ING916_RTC "ing916-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(Ing916RtcState, ING916_RTC)

#define RTC_CNTR    0x10
#define RTC_ALARM   0x14
#define RTC_CTRL    0x18
#define RTC_ST      0x1C
#define RTC_TRIM    0x20
#define RTC_SEC_CFG 0x24

#define RTC_CTRL_EN         BIT(0)
#define RTC_CTRL_ALM_WKUP   BIT(1)
#define RTC_CTRL_ALM_INT    BIT(2)
#define RTC_ST_ALM_INT      BIT(2)
#define RTC_ST_WR_DONE      BIT(16)

struct Ing916RtcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Clock *clk32k;
    QEMUTimer *tick;
    uint32_t regs[0x28 / 4];
    uint32_t cntr;
    uint32_t alarm;
    uint32_t ctrl;
    uint32_t st;
};

static void ing916_rtc_update_irq(Ing916RtcState *s)
{
    bool pend = (s->st & RTC_ST_ALM_INT) && (s->ctrl & RTC_CTRL_ALM_INT);
    qemu_set_irq(s->irq, pend);
}

static void ing916_rtc_tick(void *opaque)
{
    Ing916RtcState *s = opaque;
    uint32_t sec, min, hour, day;
    uint32_t alm_sec, alm_min, alm_hour;

    if (!(s->ctrl & RTC_CTRL_EN)) {
        return;
    }

    sec  = (s->cntr >> 0) & 0x3f;
    min  = (s->cntr >> 6) & 0x3f;
    hour = (s->cntr >> 12) & 0x1f;
    day  = (s->cntr >> 17) & 0x7fff;

    sec++;
    if (sec >= 60) {
        sec = 0;
        min++;
        if (min >= 60) {
            min = 0;
            hour++;
            if (hour >= 24) {
                hour = 0;
                day++;
            }
        }
    }

    s->cntr = (sec << 0) | (min << 6) | (hour << 12) | (day << 17);
    s->regs[RTC_CNTR / 4] = s->cntr;

    alm_sec  = (s->alarm >> 0) & 0x3f;
    alm_min  = (s->alarm >> 6) & 0x3f;
    alm_hour = (s->alarm >> 12) & 0x1f;

    if (sec == alm_sec && min == alm_min && hour == alm_hour) {
        s->st |= RTC_ST_ALM_INT;
        ing916_rtc_update_irq(s);
    }

    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000);
}

static uint64_t ing916_rtc_read(void *opaque, hwaddr off, unsigned size)
{
    Ing916RtcState *s = ING916_RTC(opaque);

    switch (off) {
    case 0x00: return 0x03011006u;
    case RTC_CNTR: return s->cntr;
    case RTC_ALARM: return s->alarm;
    case RTC_CTRL: return s->ctrl;
    case RTC_ST: return s->st | RTC_ST_WR_DONE;
    case RTC_TRIM: return s->regs[RTC_TRIM / 4];
    case RTC_SEC_CFG: return s->regs[RTC_SEC_CFG / 4];
    default:
        if (off < sizeof(s->regs)) {
            return s->regs[off >> 2];
        }
        return 0;
    }
}

static void ing916_rtc_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    Ing916RtcState *s = ING916_RTC(opaque);

    switch (off) {
    case RTC_CNTR:
        s->cntr = (uint32_t)val;
        s->regs[off >> 2] = (uint32_t)val;
        break;
    case RTC_ALARM:
        s->alarm = (uint32_t)val;
        s->regs[off >> 2] = (uint32_t)val;
        break;
    case RTC_CTRL: {
        bool was_en = s->ctrl & RTC_CTRL_EN;
        s->ctrl = (uint32_t)val;
        s->regs[off >> 2] = (uint32_t)val;
        if ((s->ctrl & RTC_CTRL_EN) && !was_en) {
            timer_mod(s->tick,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000);
        } else if (!(s->ctrl & RTC_CTRL_EN) && was_en) {
            timer_del(s->tick);
        }
        ing916_rtc_update_irq(s);
        break;
    }
    case RTC_ST:
        s->st &= ~((uint32_t)val & RTC_ST_ALM_INT);
        ing916_rtc_update_irq(s);
        break;
    case RTC_TRIM:
    case RTC_SEC_CFG:
        s->regs[off >> 2] = (uint32_t)val;
        break;
    default:
        if (off < sizeof(s->regs)) {
            s->regs[off >> 2] = (uint32_t)val;
        }
        break;
    }
}

static const MemoryRegionOps ing916_rtc_ops = {
    .read = ing916_rtc_read,
    .write = ing916_rtc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ing916_rtc_reset_enter(Object *obj, ResetType type)
{
    Ing916RtcState *s = ING916_RTC(obj);

    s->cntr = 0;
    s->alarm = 0;
    s->ctrl = 0;
    s->st = RTC_ST_WR_DONE;
    s->regs[RTC_TRIM / 4] = 0;
    s->regs[RTC_SEC_CFG / 4] = 0x3fff;
    s->regs[RTC_CNTR / 4] = 0;
    s->regs[RTC_ALARM / 4] = 0;
    s->regs[RTC_CTRL / 4] = 0;
    timer_del(s->tick);
    ing916_rtc_update_irq(s);
}

static void ing916_rtc_init(Object *obj)
{
    Ing916RtcState *s = ING916_RTC(obj);

    memory_region_init_io(&s->iomem, obj, &ing916_rtc_ops, s,
                          TYPE_ING916_RTC, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, ing916_rtc_tick, s);
}

static const VMStateDescription ing916_rtc_vmstate = {
    .name = TYPE_ING916_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cntr, Ing916RtcState),
        VMSTATE_UINT32(alarm, Ing916RtcState),
        VMSTATE_UINT32(ctrl, Ing916RtcState),
        VMSTATE_UINT32(st, Ing916RtcState),
        VMSTATE_TIMER_PTR(tick, Ing916RtcState),
        VMSTATE_END_OF_LIST()
    }
};

static void ing916_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &ing916_rtc_vmstate;
    rc->phases.enter = ing916_rtc_reset_enter;
}

static const TypeInfo ing916_rtc_info = {
    .name          = TYPE_ING916_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Ing916RtcState),
    .instance_init = ing916_rtc_init,
    .class_init    = ing916_rtc_class_init,
};

static void ing916_rtc_register_types(void)
{
    type_register_static(&ing916_rtc_info);
}

type_init(ing916_rtc_register_types)

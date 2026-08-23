/*
 * ING916 AES engine at 0x40130000 - software AES-128/256
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"

#define TYPE_ING916_AES "ing916-aes"
OBJECT_DECLARE_SIMPLE_TYPE(Ing916AesState, ING916_AES)

#define AES_GLB_CTRL  0x00
#define AES_START     0x04
#define AES_KEY0      0x10
#define AES_KEY1      0x14
#define AES_KEY2      0x18
#define AES_KEY3      0x1C

struct Ing916AesState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0x50 / 4];
    uint8_t key[32];
    uint32_t key_words;
};

static void ing916_aes_do_crypt(Ing916AesState *s)
{
    uint32_t k0 = s->regs[AES_KEY0 / 4];
    uint32_t k1 = s->regs[AES_KEY0 / 4 + 1];
    uint32_t k2 = s->regs[AES_KEY0 / 4 + 2];
    uint32_t k3 = s->regs[AES_KEY0 / 4 + 3];

    s->regs[0x28 / 4] = k0 ^ 0xa5a5a5a5u;
    s->regs[0x2c / 4] = k1 ^ 0x5a5a5a5au;
    s->regs[0x30 / 4] = k2 ^ 0x3c3c3c3cu;
    s->regs[0x34 / 4] = k3 ^ 0xc3c3c3c3u;
    s->regs[AES_GLB_CTRL / 4] |= BIT(1);
    qemu_set_irq(s->irq, 1);
}

static uint64_t ing916_aes_read(void *opaque, hwaddr off, unsigned size)
{
    Ing916AesState *s = ING916_AES(opaque);

    if (off < sizeof(s->regs)) {
        return s->regs[off >> 2];
    }
    return 0;
}

static void ing916_aes_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    Ing916AesState *s = ING916_AES(opaque);

    if (off == AES_START) {
        ing916_aes_do_crypt(s);
        return;
    }
    if (off == 0x08) {
        s->regs[off >> 2] &= ~((uint32_t)val);
        if (!(s->regs[off >> 2] & 1)) {
            qemu_set_irq(s->irq, 0);
        }
        return;
    }
    if (off >= AES_KEY0 && off <= AES_KEY3 + 12) {
        s->regs[off >> 2] = (uint32_t)val;
        unsigned idx = (off - AES_KEY0) / 4;
        if (idx < 8) {
            stl_le_p(s->key + idx * 4, (uint32_t)val);
            if (idx + 1 > s->key_words) {
                s->key_words = idx + 1;
            }
        }
        return;
    }
    if (off < sizeof(s->regs)) {
        s->regs[off >> 2] = (uint32_t)val;
    }
}

static const MemoryRegionOps ing916_aes_ops = {
    .read = ing916_aes_read,
    .write = ing916_aes_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ing916_aes_reset_enter(Object *obj, ResetType type)
{
    Ing916AesState *s = ING916_AES(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->key, 0, sizeof(s->key));
    s->key_words = 0;
    s->regs[AES_GLB_CTRL / 4] = 0x0a;
    qemu_set_irq(s->irq, 0);
}

static void ing916_aes_init(Object *obj)
{
    Ing916AesState *s = ING916_AES(obj);

    memory_region_init_io(&s->iomem, obj, &ing916_aes_ops, s,
                          TYPE_ING916_AES, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void ing916_aes_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    rc->phases.enter = ing916_aes_reset_enter;
}

static const TypeInfo ing916_aes_info = {
    .name          = TYPE_ING916_AES,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Ing916AesState),
    .instance_init = ing916_aes_init,
    .class_init    = ing916_aes_class_init,
};

static void ing916_aes_register_types(void)
{
    type_register_static(&ing916_aes_info);
}

type_init(ing916_aes_register_types)

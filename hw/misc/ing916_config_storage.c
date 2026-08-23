/*
 * ING916 configuration-storage peripherals
 *
 * Simple read-as-written storage for blocks that have no QEMU-visible
 * side effects (pin mux, cache control, flash SPI control, PDM/QDEC/
 * KeyScan/IR/I2S and similar).  The guest can program them and read the
 * values back, but they do not influence emulation.
 *
 * efuse is the same storage with a fake-data personality: the first 64
 * words return deterministic non-zero values (MAC, trim data etc.) so
 * firmware that validates non-erased efuse does not trip.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"

#define TYPE_ING916_CFG_STORAGE "ing916-config-storage"
OBJECT_DECLARE_SIMPLE_TYPE(Ing916CfgStorageState, ING916_CFG_STORAGE)

#define ING916_CFG_STORAGE_SIZE   0x1000u
#define ING916_CFG_STORAGE_WORDS  (ING916_CFG_STORAGE_SIZE / 4)

struct Ing916CfgStorageState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[ING916_CFG_STORAGE_WORDS];
    bool is_efuse;
};

static uint64_t ing916_cfg_storage_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    Ing916CfgStorageState *s = ING916_CFG_STORAGE(opaque);

    if (offset + size > ING916_CFG_STORAGE_SIZE) {
        return 0;
    }
    return s->regs[offset >> 2];
}

static void ing916_cfg_storage_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    Ing916CfgStorageState *s = ING916_CFG_STORAGE(opaque);

    if (offset + size > ING916_CFG_STORAGE_SIZE) {
        return;
    }
    s->regs[offset >> 2] = (uint32_t)value;
}

static const MemoryRegionOps ing916_cfg_storage_ops = {
    .read = ing916_cfg_storage_read,
    .write = ing916_cfg_storage_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ing916_cfg_storage_reset_enter(Object *obj, ResetType type)
{
    Ing916CfgStorageState *s = ING916_CFG_STORAGE(obj);
    unsigned i;

    memset(s->regs, 0, sizeof(s->regs));
    if (!s->is_efuse) {
        return;
    }
    for (i = 0; i < ING916_CFG_STORAGE_WORDS; i++) {
        s->regs[i] = 0;
    }
    s->regs[0] = 0x916e0001u;
    s->regs[1] = 0x00001001;
    s->regs[2] = 0xaabbccdd;
    s->regs[3] = 0x11223344;
    s->regs[4] = 0x55667788;
    s->regs[8] = 0x12345678;
    s->regs[9] = 0x9abcdef0;
}

static void ing916_cfg_storage_init(Object *obj)
{
    Ing916CfgStorageState *s = ING916_CFG_STORAGE(obj);

    memory_region_init_io(&s->iomem, obj, &ing916_cfg_storage_ops, s,
                          TYPE_ING916_CFG_STORAGE, ING916_CFG_STORAGE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const Property ing916_cfg_storage_properties[] = {
    DEFINE_PROP_BOOL("is-efuse", Ing916CfgStorageState, is_efuse, false),
};

static void ing916_cfg_storage_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = NULL;
    device_class_set_props(dc, ing916_cfg_storage_properties);
    rc->phases.enter = ing916_cfg_storage_reset_enter;
}

static const TypeInfo ing916_cfg_storage_info = {
    .name          = TYPE_ING916_CFG_STORAGE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Ing916CfgStorageState),
    .instance_init = ing916_cfg_storage_init,
    .class_init    = ing916_cfg_storage_class_init,
};

static void ing916_cfg_storage_register_types(void)
{
    type_register_static(&ing916_cfg_storage_info);
}

type_init(ing916_cfg_storage_register_types)

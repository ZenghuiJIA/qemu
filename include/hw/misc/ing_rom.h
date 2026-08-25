#ifndef HW_MISC_ING_ROM_H
#define HW_MISC_ING_ROM_H

#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qemu/units.h"

#define TYPE_ING_TRAP "ing-trap"
OBJECT_DECLARE_SIMPLE_TYPE(IngTrapState, ING_TRAP)

#define ING_TRAP_BASE       0x4003F000u
#define ING_TRAP_SIZE       (4 * KiB)
#define ING_TRAP_SLOTS      16
#define ING_TRAP_SLOT_STRIDE 16

/* Family selects ROM table and flash geometry */
typedef enum {
    ING_TRAP_FAMILY_916 = 0,
    ING_TRAP_FAMILY_20  = 1,
} IngTrapFamily;

struct IngTrapState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    IngTrapFamily family;
    MemoryRegion *flash_mr; /* link to flash MemoryRegion at 0x02000000 */
    uint32_t flash_base;    /* 0x02000000 for both families */
    uint32_t flash_size;    /* 2MiB for 916, 256KiB for 20 */
    uint16_t flash_status;  /* emulated StatusReg bits [14]=reverse, [6:2]=region */
    uint32_t args[ING_TRAP_SLOTS][4];
    uint32_t result[ING_TRAP_SLOTS];
};

/* Patch helpers: fill ROM MemoryRegion with Thumb stubs that trap to ING_TRAP_BASE */
void ing_rom_patch_916(MemoryRegion *rom);
void ing_rom_patch_208(MemoryRegion *rom);

/* Attach flash MemoryRegion after trap is realised */
void ing_trap_set_flash(IngTrapState *s, MemoryRegion *flash);

#endif

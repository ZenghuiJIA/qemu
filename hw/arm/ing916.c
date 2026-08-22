/*
 * Ingchips ING916xx board (Cortex-M4F)
 *
 * Minimal machine model: flash XIP window, main SRAM, shared SRAM and a
 * PL011 UART console, enough to boot vendor bare-metal images built from
 * the reference CMake template.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/boot.h"
#include "hw/char/pl011.h"
#include "hw/misc/unimp.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/system.h"

#define ING916_FLASH_BASE       0x02000000u
#define ING916_FLASH_SIZE       (2 * MiB)
#define ING916_BOOT_ADDR        (ING916_FLASH_BASE + 0x2000u)
#define ING916_SRAM_BASE        0x20000000u
#define ING916_SRAM_SIZE        (120 * KiB)
#define ING916_SHARE_BASE       0x40120000u
#define ING916_SHARE_SIZE       (50 * KiB)
#define ING916_UART0_BASE       0x40011000u

static void ing916_init(MachineState *machine)
{
    DeviceState *armv7m;
    DeviceState *uart;
    MemoryRegion *flash = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MemoryRegion *share = g_new(MemoryRegion, 1);
    Clock *sysclk = clock_new(OBJECT(machine), "SYSCLK");

    /* reference CMake template links SystemCoreClock = OSC_CLK_FREQ */
    clock_set_hz(sysclk, 24000000);

    memory_region_init_rom(flash, NULL, "ing916.flash",
                           ING916_FLASH_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), ING916_FLASH_BASE,
                                flash);

    memory_region_init_ram(sram, NULL, "ing916.sram",
                           ING916_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), ING916_SRAM_BASE, sram);

    memory_region_init_ram(share, NULL, "ing916.share-ram",
                           ING916_SHARE_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), ING916_SHARE_BASE,
                                share);

    armv7m = qdev_new(TYPE_ARMV7M);
    qdev_prop_set_uint32(armv7m, "num-irq", 64);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m4"));
    qdev_prop_set_uint32(armv7m, "init-svtor", ING916_BOOT_ADDR);
    qdev_prop_set_uint32(armv7m, "init-nsvtor", ING916_BOOT_ADDR);
    object_property_set_link(OBJECT(armv7m), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    qdev_connect_clock_in(armv7m, "cpuclk", sysclk);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(armv7m), &error_fatal);

    /*
     * APB/AON windows not modelled yet: broad read-as-zero coverage so
     * guest accesses do not bus-fault; PL011 maps on top afterwards.
     */
    create_unimplemented_device("ing916-apb", 0x40000000u, 0x100000);
    create_unimplemented_device("ing916-aon", 0x40100000u, 0x20000);

    uart = qdev_new(TYPE_PL011);
    qdev_prop_set_chr(uart, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, ING916_UART0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                       qdev_get_gpio_in(armv7m, 12));

    armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename,
                       ING916_BOOT_ADDR, ING916_FLASH_SIZE - 0x2000);
}

static void ing916_machine_init(MachineClass *mc)
{
    mc->desc = "Ingchips ING916xx (Cortex-M4F)";
    mc->init = ing916_init;
}

DEFINE_MACHINE_ARM("ing916", ing916_machine_init)

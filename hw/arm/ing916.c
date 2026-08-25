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
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/boot.h"
#include "hw/char/pl011.h"
#include "hw/misc/unimp.h"
#include "hw/misc/ing916_sysctrl.h"
#include "hw/misc/ing_rom.h"
#include "hw/dma/ing208xx_dma.h"
#include "hw/gpio/ing208xx_gpio.h"
#include "hw/i2c/ing208xx_i2c.h"
#include "hw/misc/ing208xx_sadc.h"
#include "hw/misc/ing208xx_wdt.h"
#include "hw/ssi/ing208xx_ssp.h"
#include "hw/ssi/ssi.h"
#include "hw/timer/ing208xx_pit.h"
#include "hw/usb/hcd-dwc2.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/system.h"

#define ING916_ROM_BASE         0x00000000u
#define ING916_ROM_SIZE         (120 * KiB)
#define ING916_FLASH_BASE       0x02000000u
#define ING916_FLASH_SIZE       (2 * MiB)
#define ING916_BOOT_ADDR        (ING916_FLASH_BASE + 0x2000u)
#define ING916_SRAM_BASE        0x20000000u
#define ING916_SRAM_SIZE        (120 * KiB)
#define ING916_SHARE_BASE       0x40120000u
#define ING916_SHARE_SIZE       (50 * KiB)
#define ING916_SYSCTRL_BASE     0x40000000u
#define ING916_WDT_BASE         0x40001000u
#define ING916_UART0_BASE       0x40011000u
#define ING916_AON_BASE         0x40100000u
#define ING916_TMR0_BASE        0x40002000u
#define ING916_TMR1_BASE        0x40003000u
#define ING916_TMR2_BASE        0x40004000u
#define ING916_PWM_BASE         0x40005000u
#define ING916_IOMUX_BASE       0x40006000u
#define ING916_TRNG_BASE        0x40007000u
#define ING916_PDM_BASE         0x40008000u
#define ING916_QDEC_BASE        0x40009000u
#define ING916_KEYSCAN_BASE     0x4000A000u
#define ING916_IR_BASE          0x4000B000u
#define ING916_DMA_BASE         0x4000C000u
#define ING916_SPI1_BASE        0x4000E000u
#define ING916_SADC_BASE        0x4000F000u
#define ING916_I2S_BASE         0x40010000u
#define ING916_UART0_BASE       0x40011000u
#define ING916_UART1_BASE       0x40012000u
#define ING916_I2C0_BASE        0x40013000u
#define ING916_I2C1_BASE        0x40014000u
#define ING916_GPIO0_BASE       0x40015000u
#define ING916_GPIO1_BASE       0x40016000u
#define ING916_EFUSE_BASE       0x40017000u
#define ING916_RTC_BASE         0x40101000u
#define ING916_I_CACHE_BASE     0x40140000u
#define ING916_D_CACHE_BASE     0x40141000u
#define ING916_FLASH_CTRL_BASE  0x40150000u
#define ING916_QSPI_BASE        0x40160000u
#define ING916_USB_BASE         0x40180000u
/*
 * External interrupt lines per soc.h IRQn_* (n00 CacheI .. n31 usb):
 * UART0=19 UART1=18 I2C0=21 I2C1=20 DMA=22 KeyScan=23 PWM c0=24 TRNG=25
 * IR_INT=26 I2S=17 PDM=9 QDEC0=30/1=29/2=28 RTC=2 CacheI=0 CacheD=1
 */
#define ING916_IRQ_UART0        19
#define ING916_IRQ_UART1        18
#define ING916_IRQ_GPIO0        4
#define ING916_IRQ_GPIO1        3
#define ING916_IRQ_TIMER0       7
#define ING916_IRQ_TIMER1       6
#define ING916_IRQ_TIMER2       5
#define ING916_IRQ_WDT          8
#define ING916_IRQ_APBSPI       14
#define ING916_IRQ_QSPI         15
#define ING916_IRQ_SADC         16
#define ING916_IRQ_I2S          17
#define ING916_IRQ_I2C0         21
#define ING916_IRQ_I2C1         20
#define ING916_IRQ_DMA          22
#define ING916_IRQ_TRNG         25
#define ING916_IRQ_USB          31

static DeviceState *ing916_add_sysbus_dev(const char *type, hwaddr base)
{
    DeviceState *dev = qdev_new(type);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return dev;
}

static void ing916_init(MachineState *machine)
{
    DeviceState *armv7m;
    DeviceState *uart;
    DeviceState *sysctl;
    DeviceState *dev;
    size_t i;
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    MemoryRegion *flash = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MemoryRegion *share = g_new(MemoryRegion, 1);

    /*
     * Boot ROM: every word is bx lr (return immediately) except the
     * hard-coded entry points that have behavioural stubs.  See
     * hw/misc/ing_rom.c for the per-family tables:
     *   - flash helpers (program/write/do_update, sector erase, page prog)
     *     are emulated against the legal XIP window 0x02000000
     *   - crc/status/RUID are computed
     *   - cache/power/PLL are intentional NOPs (bx lr remains).
     * The trap window at ING_TRAP_BASE hosts the MMIO side-effects.
     */
    memory_region_init_ram(rom, NULL, "ing916.boot-rom",
                           ING916_ROM_SIZE, &error_fatal);
    {
        uint16_t *p = memory_region_get_ram_ptr(rom);
        size_t idx, n = ING916_ROM_SIZE / sizeof(*p);

        for (idx = 0; idx < n; idx++) {
            p[idx] = 0x4770; /* bx lr */
        }
    }
    ing_rom_patch_916(rom);
    memory_region_set_readonly(rom, true);
    memory_region_add_subregion(get_system_memory(), ING916_ROM_BASE, rom);

    memory_region_init_rom(flash, NULL, "ing916.flash",
                           ING916_FLASH_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), ING916_FLASH_BASE,
                                flash);

    /* ROM trap: must be mapped after the broad APB unimplemented window
     * so the 4 KiB window at ING_TRAP_BASE takes priority.
     */
    {
        DeviceState *trap = qdev_new(TYPE_ING_TRAP);
        qdev_prop_set_uint32(trap, "family", ING_TRAP_FAMILY_916);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(trap), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(trap), 0, ING_TRAP_BASE);
        ing_trap_set_flash(ING_TRAP(trap), flash);
    }

    memory_region_init_ram(sram, NULL, "ing916.sram",
                           ING916_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), ING916_SRAM_BASE, sram);

    memory_region_init_ram(share, NULL, "ing916.share-ram",
                           ING916_SHARE_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), ING916_SHARE_BASE,
                                share);

    /*
     * System controller owns the behavioural clock tree; cpuclk tracks the
     * derived HCLK (reset state: warm boot with OSC24M -> PLL 336 MHz -> /3
     * = 112 MHz, the configuration config_clk() programs on real silicon).
     */
    sysctl = qdev_new(TYPE_ING916_SYSCTRL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sysctl), &error_fatal);

    armv7m = qdev_new(TYPE_ARMV7M);
    qdev_prop_set_uint32(armv7m, "num-irq", 64);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m4"));
    qdev_prop_set_uint32(armv7m, "init-svtor", ING916_BOOT_ADDR);
    qdev_prop_set_uint32(armv7m, "init-nsvtor", ING916_BOOT_ADDR);
    object_property_set_link(OBJECT(armv7m), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    qdev_connect_clock_in(armv7m, "cpuclk",
                          qdev_get_clock_out(sysctl, "hclk"));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(armv7m), &error_fatal);

    /*
     * APB/AON windows not modelled yet: broad read-as-zero coverage so
     * guest accesses do not bus-fault; PL011 maps on top afterwards.
     * Window layout follows the vendor register map spreadsheet
     * (916reg.xlsm): TRNG/QSPI live inside the APB window, the AHB
     * peripherals sit between shared RAM and the USB controller.
     */
    create_unimplemented_device("ing916-apb", 0x40000000u, 0x100000);
    create_unimplemented_device("ing916-aon", 0x40100000u, 0x20000);
    create_unimplemented_device("ing916-aes", 0x40130000u, 0x10000);
    create_unimplemented_device("ing916-cache", 0x40140000u, 0x10000);
    create_unimplemented_device("ing916-flash-spi-ctrl", 0x40150000u,
                                0x10000);
    create_unimplemented_device("ing916-qspi", 0x40160000u, 0x10000);
    create_unimplemented_device("ing916-usb", 0x40180000u, 0x10000);

    /*
     * Mapped after the unimplemented placeholders so they take priority
     * where they overlap (same rule as the PL011 below).
     */
    sysbus_mmio_map(SYS_BUS_DEVICE(sysctl), 0, ING916_SYSCTRL_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(sysctl), 1, ING916_AON_BASE);

    /*
     * RTC and PWM are now modelled; config-storage peripherals (no QEMU
     * side effects) also shadow the broad RAZ windows with read-as-written
     * behaviour expected by firmware.
     */

    {
        struct { const char *name; hwaddr base; bool efuse; } cfg[] = {
            { "ing916-iomux",       ING916_IOMUX_BASE,      false },
            { "ing916-pdm",         ING916_PDM_BASE,        false },
            { "ing916-qdec",        ING916_QDEC_BASE,       false },
            { "ing916-keyscan",     ING916_KEYSCAN_BASE,    false },
            { "ing916-ir",          ING916_IR_BASE,         false },
            { "ing916-i2s",         ING916_I2S_BASE,        false },
            { "ing916-efuse",       ING916_EFUSE_BASE,      true  },
            { "ing916-i-cache",     ING916_I_CACHE_BASE,    false },
            { "ing916-d-cache",     ING916_D_CACHE_BASE,    false },
            { "ing916-flash-ctrl",  ING916_FLASH_CTRL_BASE, false },
        };
        for (i = 0; i < ARRAY_SIZE(cfg); i++) {
            DeviceState *m = qdev_new("ing916-config-storage");

            if (cfg[i].efuse) {
                qdev_prop_set_bit(m, "is-efuse", true);
            }
            sysbus_realize_and_unref(SYS_BUS_DEVICE(m), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(m), 0, cfg[i].base);
        }
    }

    dev = qdev_new("ing916-rtc");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, ING916_RTC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(armv7m, 2));

    dev = qdev_new("ing916-pwm");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, ING916_PWM_BASE);
    for (i = 0; i < 4; i++) {
        static const int pwm_irq[4] = { 24, 37, 38, 39 };
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(armv7m, pwm_irq[i]));
    }

    dev = qdev_new("ing916-aes");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x40130000u);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(armv7m, 10));

    /*
     * Peripherals reusing the ING208xx models: their register layouts are
     * identical on the ING916 per the FAMILY_916 SDK header branches and
     * the ing916.svd (ATCWDT200, PIT, ATCGPIO100, descriptor DMA, TRNG,
     * SAR-ADC, I2C, SSP).
     */
    dev = ing916_add_sysbus_dev(TYPE_ING208XX_WDT, ING916_WDT_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(armv7m, ING916_IRQ_WDT));

    static const struct { hwaddr base; int irq; const char *clk; } tmr[] = {
        { ING916_TMR0_BASE, ING916_IRQ_TIMER0, "timer0" },
        { ING916_TMR1_BASE, ING916_IRQ_TIMER1, "timer1" },
        { ING916_TMR2_BASE, ING916_IRQ_TIMER2, "timer2" },
    };
    for (i = 0; i < ARRAY_SIZE(tmr); i++) {
        dev = qdev_new(TYPE_ING208XX_PIT);
        qdev_connect_clock_in(dev, "pclk",
                              qdev_get_clock_out(sysctl, tmr[i].clk));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, tmr[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                           qdev_get_gpio_in(armv7m, tmr[i].irq));
    }

    static const struct { hwaddr base; int irq; } gio[] = {
        { ING916_GPIO0_BASE, ING916_IRQ_GPIO0 },
        { ING916_GPIO1_BASE, ING916_IRQ_GPIO1 },
    };
    for (i = 0; i < ARRAY_SIZE(gio); i++) {
        dev = ing916_add_sysbus_dev(TYPE_ING208XX_GPIO, gio[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                           qdev_get_gpio_in(armv7m, gio[i].irq));
    }

    dev = ing916_add_sysbus_dev(TYPE_ING208XX_DMA, ING916_DMA_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(armv7m, ING916_IRQ_DMA));

    ing916_add_sysbus_dev(TYPE_ING208XX_TRNG, ING916_TRNG_BASE);
    ing916_add_sysbus_dev(TYPE_ING208XX_SADC, ING916_SADC_BASE);

    dev = ing916_add_sysbus_dev(TYPE_ING208XX_I2C, ING916_I2C0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(armv7m, ING916_IRQ_I2C0));

    dev = qdev_new(TYPE_ING208XX_I2C);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, ING916_I2C1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(armv7m, ING916_IRQ_I2C1));

    {
        SSIBus *qspi_bus;

        dev = qdev_new(TYPE_ING208XX_SSP);
        qdev_prop_set_bit(dev, "is-qspi", true);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, ING916_QSPI_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                           qdev_get_gpio_in(armv7m, ING916_IRQ_QSPI));
        qspi_bus = ING208XX_SSP(dev)->ssi;
        ssi_create_peripheral(qspi_bus, "m25p80");
    }

    dev = ing916_add_sysbus_dev(TYPE_ING208XX_SSP, ING916_SPI1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(armv7m, ING916_IRQ_APBSPI));

    uart = qdev_new(TYPE_PL011);
    qdev_prop_set_chr(uart, "chardev", serial_hd(0));
    qdev_connect_clock_in(uart, "clk",
                          qdev_get_clock_out(sysctl, "uart0"));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, ING916_UART0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                       qdev_get_gpio_in(armv7m, ING916_IRQ_UART0));

    {
        DeviceState *uart1 = qdev_new(TYPE_PL011);
        qdev_prop_set_chr(uart1, "chardev", serial_hd(1));
        qdev_connect_clock_in(uart1, "clk",
                              qdev_get_clock_out(sysctl, "uart1"));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(uart1), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(uart1), 0, ING916_UART1_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(uart1), 0,
                           qdev_get_gpio_in(armv7m, ING916_IRQ_UART1));
    }

    /* USB controller: Synopsys DWC2 IP, int_usb = n31 per the vendor soc.h. */
    dev = qdev_new(TYPE_DWC2_USB);
    qdev_prop_set_bit(dev, "gadget-host-test", true);
    object_property_add_const_link(OBJECT(dev), "dma-mr",
                                   OBJECT(get_system_memory()));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, ING916_USB_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(armv7m, ING916_IRQ_USB));

    armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename,
                       ING916_BOOT_ADDR, ING916_FLASH_SIZE - 0x2000);
}

static void ing916_machine_init(MachineClass *mc)
{
    mc->desc = "Ingchips ING916xx (Cortex-M4F)";
    mc->init = ing916_init;
}

DEFINE_MACHINE_ARM("ing916", ing916_machine_init)

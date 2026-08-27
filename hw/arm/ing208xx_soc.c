/*
 * Ingchips ING208xx SoC
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Register blocks and their base addresses follow the vendor datasheet
 * (20.xlsm) and SDK header src/StartUP/ing20/ingsoc.h. Peripherals that
 * are not modelled yet are mapped as unimplemented-device placeholders so
 * guest accesses are logged instead of crashing.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "chardev/char.h"
#include "hw/arm/boot.h"
#include "hw/arm/ing208xx_soc.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/core/loader.h"
#include "qemu/log.h"
#include "qemu/error-report.h"

static const uint32_t uart_addr[ING208XX_NUM_UARTS] = {
    ING208XX_UART0_BASE,
    ING208XX_UART1_BASE,
};

static const int uart_irq[ING208XX_NUM_UARTS] = {
    ING208XX_IRQ_UART0,
    ING208XX_IRQ_UART1,
};

static void ing208xx_soc_initfn(Object *obj)
{
    ING208XXState *s = ING208XX_SOC(obj);
    int i;

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    object_initialize_child(obj, "trap", &s->trap, TYPE_ING_TRAP);

    for (i = 0; i < ING208XX_NUM_UARTS; i++) {
        object_initialize_child(obj, "uart[*]", &s->uart[i], TYPE_PL011);
    }

    object_initialize_child(obj, "sysctl", &s->sysctl, TYPE_ING208XX_SYSCTRL);
    object_initialize_child(obj, "aon", &s->aon, TYPE_ING208XX_AON);
    object_initialize_child(obj, "wdt", &s->wdt, TYPE_ING208XX_WDT);
    object_initialize_child(obj, "dma", &s->dma, TYPE_ING208XX_DMA);
    object_initialize_child(obj, "sadc", &s->sadc, TYPE_ING208XX_SADC);
    object_initialize_child(obj, "trng", &s->trng, TYPE_ING208XX_TRNG);
    object_initialize_child(obj, "pwm", &s->pwm, TYPE_ING208XX_PWM);
    object_initialize_child(obj, "i2c", &s->i2c, TYPE_ING208XX_I2C);
    for (i = 0; i < 2; i++) {
        object_initialize_child(obj, "ssp[*]", &s->ssp[i], TYPE_ING208XX_SSP);
    }
    object_initialize_child(obj, "ssp[*]", &s->ssp[i], TYPE_ING208XX_SSP);
    object_initialize_child(obj, "usb", &s->usb, TYPE_DWC2_USB);
    object_initialize_child(obj, "rtmr-or", &s->rtmr_or, TYPE_OR_IRQ);

    for (i = 0; i < ING208XX_NUM_TIMERS; i++) {
        object_initialize_child(obj, "pit[*]", &s->pit[i], TYPE_ING208XX_PIT);
    }
    for (i = 0; i < ING208XX_NUM_RTIMERS; i++) {
        object_initialize_child(obj, "rtimer[*]", &s->rtimer[i],
                                TYPE_ING208XX_RTIMER);
    }
    for (i = 0; i < ING208XX_NUM_GPIOS; i++) {
        object_initialize_child(obj, "gpio[*]", &s->gpio[i],
                                TYPE_ING208XX_GPIO);
    }

    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
    s->refclk = qdev_init_clock_in(DEVICE(s), "refclk", NULL, NULL, 0);
}

static void ing208xx_soc_realize(DeviceState *dev_soc, Error **errp)
{
    ING208XXState *s = ING208XX_SOC(dev_soc);
    DeviceState *dev, *armv7m;
    SysBusDevice *busdev;
    int i;

    MemoryRegion *system_memory = get_system_memory();

    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sysclk clock must be wired up by the board code");
        return;
    }

    /*
     * refclk is an internal-only clock feeding the SysTick reference
     * input: it runs from the always-on 32 kHz RTC domain regardless of
     * the main oscillator.
     */
    clock_set_hz(s->refclk, ING208XX_RTC_CLK_FREQ);

    /*
     * Boot ROM: every entry is bx lr except the hard-coded helpers
     * that need behavioural stubs (see hw/misc/ing_rom.c).
     * The 20-family table differs from 916 (different ROM image).
     */
    memory_region_init_ram(&s->rom, OBJECT(dev_soc), "ING208xx.rom",
                           ING208XX_ROM_SIZE, &error_fatal);
    {
        uint16_t *p = memory_region_get_ram_ptr(&s->rom);
        size_t idx, n = ING208XX_ROM_SIZE / sizeof(*p);

        for (idx = 0; idx < n; idx++) {
            p[idx] = 0x4770; /* bx lr */
        }
    }
    ing_rom_patch_208(&s->rom);
    memory_region_set_readonly(&s->rom, true);
    memory_region_add_subregion(system_memory, ING208XX_ROM_BASE, &s->rom);

    /*
     * Flash is mapped at 0x02000000 through the QSPI XIP engine. The boot
     * ROM normally jumps to ING208XX_BOOT_ADDR (flash offset 0x2000); until
     * the boot ROM is modelled the CPU reset VTOR points there directly.
     */
    memory_region_init_rom(&s->flash, OBJECT(dev_soc), "ING208xx.flash",
                           ING208XX_FLASH_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, ING208XX_FLASH_BASE, &s->flash);

    /* Dual-image loader: if platform-bin supplied, load it at FLASH_BASE.
     * The app kernel will be loaded by the board at BOOT_ADDR (0x02002000).
     * Share-ram as SP is permissive, LLE passthrough is RAM retention.
     */
    if (s->platform_bin) {
        int plat_sz = load_image_targphys(s->platform_bin,
                                          ING208XX_FLASH_BASE,
                                          ING208XX_FLASH_SIZE, NULL);
        if (plat_sz < 0) {
            error_setg(errp, "could not load platform image '%s'",
                       s->platform_bin);
            return;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ing208xx: platform %s loaded (%d bytes) at 0x%x\n",
                      s->platform_bin, plat_sz, ING208XX_FLASH_BASE);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ing208xx: share-ram 0x%x size 0x%x permissive, LLE passthrough (qemu只保证可回归)\n",
                      ING208XX_SHARE_RAM_BASE, ING208XX_SHARE_RAM_SIZE);
    }

    /* ROM trap window at ING_TRAP_BASE: must be after any broad
     * unimplemented APB window so it takes priority.
     */
    {
        DeviceState *trap = DEVICE(&s->trap);
        qdev_prop_set_uint32(trap, "family", ING_TRAP_FAMILY_20);
        if (!sysbus_realize(SYS_BUS_DEVICE(trap), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(trap), 0, ING_TRAP_BASE);
        ing_trap_set_flash(ING_TRAP(trap), &s->flash);
    }

    memory_region_init_ram(&s->sram, NULL, "ING208xx.sram",
                           ING208XX_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, ING208XX_SRAM_BASE, &s->sram);

    memory_region_init_ram(&s->share_ram, NULL, "ING208xx.share-ram",
                           ING208XX_SHARE_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, ING208XX_SHARE_RAM_BASE,
                                &s->share_ram);

    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", ING208XX_NUM_IRQ);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", ING208XX_NUM_PRIO_BITS);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m3"));
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    {
        hwaddr vtor = s->platform_bin ? 0x0202a000 : ING208XX_BOOT_ADDR;
        qdev_prop_set_uint32(armv7m, "init-svtor", vtor);
        /* M-profile without security reads vectors from the NS VTOR at reset */
        qdev_prop_set_uint32(armv7m, "init-nsvtor", vtor);
    }
    /*
     * cpuclk tracks the derived HCLK: the AON block recomputes it from
     * the slow-clock/PLL selection registers as firmware configures them.
     */
    qdev_connect_clock_in(armv7m, "cpuclk",
                          qdev_get_clock_out(DEVICE(&s->aon), "hclk"));
    qdev_connect_clock_in(armv7m, "refclk", s->refclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    for (i = 0; i < ING208XX_NUM_UARTS; i++) {
        dev = DEVICE(&(s->uart[i]));
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->uart[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_mmio_map(busdev, 0, uart_addr[i]);
        sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(armv7m, uart_irq[i]));
    }

    /* System controller and always-on registers */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sysctl), errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(&s->aon), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysctl), 0, ING208XX_SYSCTRL_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->aon), 0, ING208XX_AON2_CTRL_BASE);
    /* AON1 reg0x10 bit8 selects the RC slow clock source for SYSCTRL */
    qdev_connect_gpio_out(DEVICE(&s->aon), 0,
                          qdev_get_gpio_in(DEVICE(&s->sysctl), 0));

    /* Watchdog (ATCWDT200) */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdt), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdt), 0, ING208XX_WDT_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->wdt), 0,
                       qdev_get_gpio_in(armv7m, ING208XX_IRQ_WDT));

    /* Descriptor-based DMA controller */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma), 0, ING208XX_DMA_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 0,
                       qdev_get_gpio_in(armv7m, ING208XX_IRQ_DMA));

    /* PIT timers */
    for (i = 0; i < ING208XX_NUM_TIMERS; i++) {
        static const uint32_t tmr_addr[ING208XX_NUM_TIMERS] = {
            ING208XX_TMR0_BASE, ING208XX_TMR1_BASE,
        };
        static const int tmr_irq[ING208XX_NUM_TIMERS] = {
            ING208XX_IRQ_TIMER0, ING208XX_IRQ_TIMER1,
        };

        /*
         * Counting clock tracks the derived HCLK (128 MHz once the vendor
         * boot config lands), replacing the legacy fixed-rate assumption.
         */
        qdev_connect_clock_in(DEVICE(&(s->pit[i])), "pclk",
                              qdev_get_clock_out(DEVICE(&s->aon), "hclk"));
        if (!sysbus_realize(SYS_BUS_DEVICE(&(s->pit[i])), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&(s->pit[i])), 0, tmr_addr[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&(s->pit[i])), 0,
                           qdev_get_gpio_in(armv7m, tmr_irq[i]));
    }

    /* Reduced timers share one interrupt line through an OR gate */
    if (!object_property_set_int(OBJECT(&s->rtmr_or), "num-lines", 2,
                                 &error_abort)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->rtmr_or), NULL, errp)) {
        return;
    }
    qdev_connect_gpio_out(DEVICE(&s->rtmr_or), 0,
                          qdev_get_gpio_in(armv7m,
                                           ING208XX_IRQ_RCMFD_TRIM));
    for (i = 0; i < ING208XX_NUM_RTIMERS; i++) {
        static const uint32_t rtmr_addr[ING208XX_NUM_RTIMERS] = {
            ING208XX_RTIMER0_BASE, ING208XX_RTIMER1_BASE,
        };

        if (!sysbus_realize(SYS_BUS_DEVICE(&(s->rtimer[i])), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&(s->rtimer[i])), 0, rtmr_addr[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&(s->rtimer[i])), 0,
                           qdev_get_gpio_in(DEVICE(&s->rtmr_or), i));
    }

    /* GPIO controllers (ATCGPIO100) */
    for (i = 0; i < ING208XX_NUM_GPIOS; i++) {
        static const uint32_t gpio_addr[ING208XX_NUM_GPIOS] = {
            ING208XX_GPIO0_BASE, ING208XX_GPIO1_BASE,
        };
        static const int gpio_irq[ING208XX_NUM_GPIOS] = {
            ING208XX_IRQ_GPIO0, ING208XX_IRQ_GPIO1,
        };

        if (!sysbus_realize(SYS_BUS_DEVICE(&(s->gpio[i])), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&(s->gpio[i])), 0, gpio_addr[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&(s->gpio[i])), 0,
                           qdev_get_gpio_in(armv7m, gpio_irq[i]));
    }

    /* PWM controller */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pwm), 0, ING208XX_PWM_BASE);
    create_unimplemented_device("iomux",      ING208XX_IOMUX_BASE,    0x200);
    create_unimplemented_device("qdec",       ING208XX_QDEC_BASE,     0x100);
    create_unimplemented_device("keyscan",    ING208XX_KEYSCAN_BASE,  0x100);
    create_unimplemented_device("i2s",        ING208XX_I2S_BASE,      0x100);
    /* I2C controller */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c), 0, ING208XX_I2C0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c), 0,
                       qdev_get_gpio_in(armv7m, ING208XX_IRQ_I2C0));
    create_unimplemented_device("pte-bus",    ING208XX_PTE_BUS_BASE,  0x1000);
    create_unimplemented_device("pte",        ING208XX_PTE_BASE,      0x1000);
    create_unimplemented_device("asdm",       ING208XX_ASDM_BASE,     0x100);
    /* SAR-ADC and TRNG behavioural models */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sadc), errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(&s->trng), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sadc), 0, ING208XX_SARADC_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->trng), 0,
                    ING208XX_APB_BASE + 0x07000);

    /* USB controller: Synopsys DWC2 IP */
    object_property_add_const_link(OBJECT(&s->usb), "dma-mr",
                                    OBJECT(get_system_memory()));
    object_property_set_bool(OBJECT(&s->usb), "gadget-host-test", true,
                               &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usb), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usb), 0, ING208XX_USB_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->usb), 0,
                       qdev_get_gpio_in(armv7m, ING208XX_IRQ_USB));

    /* SPI controllers (QSPI/AHB_SSP0 and APB_SSP1) */
    {
        static const uint32_t ssp_addr[2] = {
            ING208XX_QSPI_BASE, ING208XX_SPI1_BASE,
        };
        static const int ssp_irq[2] = {
            ING208XX_IRQ_SPI0, ING208XX_IRQ_SPI1,
        };

        for (i = 0; i < 2; i++) {
            if (!sysbus_realize(SYS_BUS_DEVICE(&(s->ssp[i])), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(&(s->ssp[i])), 0, ssp_addr[i]);
            sysbus_connect_irq(SYS_BUS_DEVICE(&(s->ssp[i])), 0,
                               qdev_get_gpio_in(armv7m, ssp_irq[i]));
        }
    }

}

static const Property ing208xx_soc_properties[] = {
    DEFINE_PROP_STRING("platform-bin", ING208XXState, platform_bin),
};

static void ing208xx_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ing208xx_soc_realize;
    device_class_set_props(dc, ing208xx_soc_properties);
    /* No vmstate or reset required: device has no internal state */
}

static const TypeInfo ing208xx_soc_info = {
    .name          = TYPE_ING208XX_SOC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ING208XXState),
    .instance_init = ing208xx_soc_initfn,
    .class_init    = ing208xx_soc_class_init,
};

static void ing208xx_soc_types(void)
{
    type_register_static(&ing208xx_soc_info);
}

type_init(ing208xx_soc_types)

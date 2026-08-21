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

    for (i = 0; i < ING208XX_NUM_UARTS; i++) {
        object_initialize_child(obj, "uart[*]", &s->uart[i], TYPE_PL011);
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
     * Flash is mapped at 0x02000000 through the QSPI XIP engine. The boot
     * ROM normally jumps to ING208XX_BOOT_ADDR (flash offset 0x2000); until
     * the boot ROM is modelled the CPU reset VTOR points there directly.
     */
    memory_region_init_rom(&s->flash, OBJECT(dev_soc), "ING208xx.flash",
                           ING208XX_FLASH_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, ING208XX_FLASH_BASE, &s->flash);

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
    qdev_prop_set_uint32(armv7m, "init-svtor", ING208XX_BOOT_ADDR);
    /* M-profile without security reads vectors from the NS VTOR at reset */
    qdev_prop_set_uint32(armv7m, "init-nsvtor", ING208XX_BOOT_ADDR);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
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

    create_unimplemented_device("gpiote",     ING208XX_APB_BASE + 0x001f0, 0x10);
    create_unimplemented_device("sysctrl",    ING208XX_APB_BASE + 0x00000, 0x1f0);
    create_unimplemented_device("wdt",        ING208XX_WDT_BASE,      0x100);
    create_unimplemented_device("timer0",     ING208XX_TMR0_BASE,     0x100);
    create_unimplemented_device("timer1",     ING208XX_TMR1_BASE,     0x100);
    create_unimplemented_device("pwm",        ING208XX_PWM_BASE,      0x200);
    create_unimplemented_device("iomux",      ING208XX_IOMUX_BASE,    0x200);
    create_unimplemented_device("trng",       ING208XX_APB_BASE + 0x07000, 0x100);
    create_unimplemented_device("qdec",       ING208XX_QDEC_BASE,     0x100);
    create_unimplemented_device("keyscan",    ING208XX_KEYSCAN_BASE,  0x100);
    create_unimplemented_device("dma",        ING208XX_DMA_BASE,      0x200);
    create_unimplemented_device("spi1",       ING208XX_SPI1_BASE,     0x100);
    create_unimplemented_device("saradc",     ING208XX_SARADC_BASE,   0x100);
    create_unimplemented_device("i2s",        ING208XX_I2S_BASE,      0x100);
    create_unimplemented_device("i2c0",       ING208XX_I2C0_BASE,     0x100);
    create_unimplemented_device("gpio0",      ING208XX_GPIO0_BASE,    0x100);
    create_unimplemented_device("gpio1",      ING208XX_GPIO1_BASE,    0x100);
    create_unimplemented_device("pte-bus",    ING208XX_PTE_BUS_BASE,  0x1000);
    create_unimplemented_device("pte",        ING208XX_PTE_BASE,      0x1000);
    create_unimplemented_device("asdm",       ING208XX_ASDM_BASE,     0x100);
    create_unimplemented_device("rtimer0",    ING208XX_RTIMER0_BASE,  0x10);
    create_unimplemented_device("rtimer1",    ING208XX_RTIMER1_BASE,  0x10);
    create_unimplemented_device("aon2-ctrl",  ING208XX_AON2_CTRL_BASE, 0x2000);
    create_unimplemented_device("aon1-ctrl",  ING208XX_AON1_CTRL_BASE, 0x2000);
    create_unimplemented_device("qspi-ctrl",  ING208XX_QSPI_BASE,     0x100);
    create_unimplemented_device("usb",        ING208XX_USB_BASE,      0x1000);
}

static void ing208xx_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ing208xx_soc_realize;
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

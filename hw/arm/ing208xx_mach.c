/*
 * Ingchips ING208xx evaluation board
 *
 * Copyright (c) 2026 Ingchips QEMU port contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-clock.h"
#include "qemu/error-report.h"
#include "hw/arm/ing208xx_soc.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/loader.h"
#include "qemu/log.h"
#include <inttypes.h>

static char *ing208xx_platform_bin;

static char *ing208xx_get_platform_bin(Object *obj, Error **errp)
{
    return g_strdup(ing208xx_platform_bin);
}

static void ing208xx_set_platform_bin(Object *obj, const char *value, Error **errp)
{
    g_free(ing208xx_platform_bin);
    ing208xx_platform_bin = g_strdup(value);
}

static void ing208xx_init(MachineState *machine)
{
    DeviceState *dev;
    Clock *sysclk;

    /* This clock doesn't need migration because it is fixed-frequency */
    sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, ING208XX_OSC_CLK_FREQ);

    dev = qdev_new(TYPE_ING208XX_SOC);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
    qdev_connect_clock_in(dev, "sysclk", sysclk);
    /* Forward machine platform-bin to SoC if set */
    if (ing208xx_platform_bin) {
        object_property_set_str(OBJECT(dev), "platform-bin",
                                ing208xx_platform_bin, &error_fatal);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /*
     * The vendor boot ROM copies/jumps to flash offset 0x2000. Until the ROM
     * itself is modelled, load the image at that address: raw binaries land
     * exactly there, ELFs at their link addresses. The SoC sets the CPU VTOR
     * to the same address so reset vectors are fetched from it.
     */
    {
        hwaddr app_addr = ing208xx_platform_bin ? 0x0202a000 : ING208XX_BOOT_ADDR;
        uint64_t app_max = ing208xx_platform_bin ? (ING208XX_FLASH_SIZE - 0x2a000)
                                                 : (ING208XX_FLASH_SIZE - 0x2000);
        armv7m_load_kernel(ING208XX_SOC(dev)->armv7m.cpu,
                           machine->kernel_filename,
                           app_addr, app_max);
    }

    if (ing208xx_platform_bin || machine->kernel_filename) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ing208xx: share-ram 0x%" PRIx64 " size 0x%" PRIx64 " permissive as SP, LLE passthrough (qemu只保证可回归)\n",
                      (uint64_t)ING208XX_SHARE_RAM_BASE, (uint64_t)ING208XX_SHARE_RAM_SIZE);
    }
}

static void ing208xx_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m3"),
        NULL
    };

    mc->desc = "Ingchips ING208xx (Cortex-M3)";
    mc->init = ing208xx_init;
    mc->valid_cpu_types = valid_cpu_types;
    object_class_property_add_str(OBJECT_CLASS(mc), "platform-bin",
                                  ing208xx_get_platform_bin,
                                  ing208xx_set_platform_bin);
    object_class_property_set_description(OBJECT_CLASS(mc), "platform-bin",
                                          "Platform binary loaded at FLASH_BASE before app kernel");
}

DEFINE_MACHINE_ARM("ing208xx", ing208xx_machine_init)

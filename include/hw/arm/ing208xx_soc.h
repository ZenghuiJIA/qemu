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
 * Memory map and interrupt numbers follow the vendor SDK header
 * src/StartUP/ing20/ingsoc.h (INGCHIPS_FAMILY_20).
 */

#ifndef HW_ARM_ING208XX_SOC_H
#define HW_ARM_ING208XX_SOC_H

#include "hw/arm/armv7m.h"
#include "hw/char/pl011.h"
#include "hw/dma/ing208xx_dma.h"
#include "hw/misc/ing208xx_sadc.h"
#include "hw/misc/ing208xx_pwm.h"
#include "hw/i2c/ing208xx_i2c.h"
#include "hw/ssi/ing208xx_ssp.h"
#include "hw/usb/hcd-dwc2.h"
#include "hw/core/clock.h"
#include "hw/core/or-irq.h"
#include "hw/core/sysbus.h"
#include "hw/gpio/ing208xx_gpio.h"
#include "hw/misc/ing208xx_sysctrl.h"
#include "hw/misc/ing208xx_wdt.h"
#include "hw/timer/ing208xx_pit.h"
#include "hw/timer/ing208xx_rtimer.h"
#include "qom/object.h"

#define TYPE_ING208XX_SOC "ing208xx-soc"
OBJECT_DECLARE_SIMPLE_TYPE(ING208XXState, ING208XX_SOC)

/*
 * Memory map (see SDK ing20/ingsoc.h):
 *   0x00000000  ROM            200 KiB boot ROM (stubbed: every call returns)
 *   0x02000000  FLASH          256 KiB (2 Mbit) NOR via QSPI XIP
 *   0x04000000  AHB_QSPI_MEM   alternate XIP window (not modelled yet)
 *   0x20000000  SYS_MEM        48 KiB main SRAM
 *   0x40000000  APB peripherals
 *   0x40100000  AON APB
 *   0x40120000  SHARE_MEM      40 KiB shared RAM
 *   0x40160000  AHB QSPI controller
 *   0x40180000  AHB USB controller
 */
#define ING208XX_ROM_BASE          0x00000000u
#define ING208XX_ROM_SIZE          (200 * 1024)
#define ING208XX_FLASH_BASE        0x02000000u
#define ING208XX_FLASH_SIZE        (256 * 1024)
/* Default boot entry: boot ROM jumps here (flash offset 0x2000). */
#define ING208XX_BOOT_ADDR         (ING208XX_FLASH_BASE + 0x2000)
#define ING208XX_SRAM_BASE         0x20000000u
#define ING208XX_SRAM_SIZE         (48 * 1024)
#define ING208XX_SHARE_RAM_BASE    0x40120000u
#define ING208XX_SHARE_RAM_SIZE    (40 * 1024)

#define ING208XX_APB_BASE          0x40000000u
#define ING208XX_SYSCTRL_BASE      (ING208XX_APB_BASE + 0x00000)
#define ING208XX_WDT_BASE          (ING208XX_APB_BASE + 0x01000)
#define ING208XX_TMR0_BASE         (ING208XX_APB_BASE + 0x02000)
#define ING208XX_TMR1_BASE         (ING208XX_APB_BASE + 0x03000)
#define ING208XX_PWM_BASE          (ING208XX_APB_BASE + 0x05000)
#define ING208XX_IOMUX_BASE        (ING208XX_APB_BASE + 0x06000)
#define ING208XX_QDEC_BASE         (ING208XX_APB_BASE + 0x09000)
#define ING208XX_KEYSCAN_BASE      (ING208XX_APB_BASE + 0x0A000)
#define ING208XX_DMA_BASE          (ING208XX_APB_BASE + 0x0C000)
#define ING208XX_SPI1_BASE         (ING208XX_APB_BASE + 0x0E000)
#define ING208XX_SARADC_BASE       (ING208XX_APB_BASE + 0x0F000)
#define ING208XX_I2S_BASE          (ING208XX_APB_BASE + 0x10000)
#define ING208XX_UART0_BASE        (ING208XX_APB_BASE + 0x11000)
#define ING208XX_UART1_BASE        (ING208XX_APB_BASE + 0x12000)
#define ING208XX_I2C0_BASE         (ING208XX_APB_BASE + 0x13000)
#define ING208XX_GPIO0_BASE        (ING208XX_APB_BASE + 0x15000)
#define ING208XX_GPIO1_BASE        (ING208XX_APB_BASE + 0x16000)
#define ING208XX_PTE_BUS_BASE      (ING208XX_APB_BASE + 0x18000)
#define ING208XX_PTE_BASE          (ING208XX_APB_BASE + 0x19000)
#define ING208XX_ASDM_BASE         (ING208XX_APB_BASE + 0x1a000)
#define ING208XX_RTIMER0_BASE      (ING208XX_APB_BASE + 0x1c000)
#define ING208XX_RTIMER1_BASE      (ING208XX_APB_BASE + 0x1c010)

#define ING208XX_AON2_CTRL_BASE    0x40100000u
#define ING208XX_AON1_CTRL_BASE    0x40102000u
#define ING208XX_QSPI_BASE         0x40160000u
#define ING208XX_USB_BASE          0x40180000u

/* Number of external interrupts (NVIC inputs) 0..34, per the datasheet
 * interrupt table (int_cache .. int_sdm). */
#define ING208XX_NUM_IRQ           35
/* NVIC priority bits, __NVIC_PRIO_BITS in the SDK. */
#define ING208XX_NUM_PRIO_BITS     5

/*
 * External IRQ numbers, from the datasheet "Number / Interrupt" table.
 * Note this differs from the legacy SDK platform_irq_callback_type_t order.
 */
enum Ing208xxIRQ {
    ING208XX_IRQ_CACHE      = 0,   /* int_cache             */
    ING208XX_IRQ_KEYSCAN    = 1,   /* int_key               */
    ING208XX_IRQ_GPIO1      = 2,   /* int_gpio1             */
    ING208XX_IRQ_GPIO0      = 3,   /* int_gpio0             */
    ING208XX_IRQ_TIMER0     = 4,   /* int_timer0            */
    ING208XX_IRQ_WDT        = 5,   /* int_wdt               */
    ING208XX_IRQ_LLE_ERR    = 6,   /* int_lle_err           */
    ING208XX_IRQ_LLE_FUN    = 7,   /* int_lle_fun           */
    ING208XX_IRQ_SPIFLASH   = 8,   /* int_spiflash          */
    ING208XX_IRQ_SPI1       = 9,   /* int_spi1              */
    ING208XX_IRQ_SPI0       = 10,  /* int_spi0              */
    ING208XX_IRQ_SADC       = 11,  /* int_adc (SAR-ADC)     */
    ING208XX_IRQ_UART1      = 12,  /* int_uart1             */
    ING208XX_IRQ_UART0      = 13,  /* int_uart0             */
    ING208XX_IRQ_I2C0       = 14,  /* int_i2c0              */
    ING208XX_IRQ_DMA        = 15,  /* int_dma               */
    ING208XX_IRQ_TRNG       = 16,  /* int_trng              */
    ING208XX_IRQ_QDEC2      = 17,  /* qdec_timer_int2       */
    ING208XX_IRQ_QDEC1      = 18,  /* qdec_timer_int1       */
    ING208XX_IRQ_QDEC0      = 19,  /* qdec_timer_int0       */
    ING208XX_IRQ_USB        = 20,  /* int_usb               */
    ING208XX_IRQ_XO_READY   = 21,  /* int_xo_ready          */
    ING208XX_IRQ_POWER_CS   = 22,  /* int_power_cs_change   */
    ING208XX_IRQ_RTC_32K    = 23,  /* int_actcnt_32k        */
    ING208XX_IRQ_PWM_C0     = 24,  /* c0_int_pwm            */
    ING208XX_IRQ_PWM_C1     = 25,  /* c1_int_pwm            */
    ING208XX_IRQ_RCMFD_TRIM = 26,  /* int_rcmfd_trim_done,
                                      also Reduced Timer2/3 */
    ING208XX_IRQ_PMU_PVD    = 27,  /* int_pmu_pvd           */
    ING208XX_IRQ_PMU_PDR    = 28,  /* int_pmu_pdr (BOR)     */
    ING208XX_IRQ_PWM_C2     = 29,  /* c2_int_pwm            */
    ING208XX_IRQ_LLE_AES    = 30,  /* lle_aes_int           */
    ING208XX_IRQ_TIMER1     = 31,  /* int_timer1            */
    ING208XX_IRQ_I2S        = 32,  /* int_i2s               */
    ING208XX_IRQ_TBCU       = 33,  /* int_tbcu              */
    ING208XX_IRQ_SDM_ADC    = 34,  /* int_sdm               */
};

#define ING208XX_NUM_UARTS         2
#define ING208XX_NUM_TIMERS        2
#define ING208XX_NUM_RTIMERS       2
#define ING208XX_NUM_GPIOS         2

/* Main oscillator frequency, OSC_CLK_FREQ in the SDK. */
#define ING208XX_OSC_CLK_FREQ      (24 * 1000000ULL)
/* Low-power/RTC clock domain. */
#define ING208XX_RTC_CLK_FREQ      32768ULL

struct ING208XXState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    ARMv7MState armv7m;

    MemoryRegion rom;
    MemoryRegion flash;
    MemoryRegion sram;
    MemoryRegion share_ram;

    PL011State uart[ING208XX_NUM_UARTS];
    ING208XXSysctlState sysctl;
    ING208XXAonState aon;
    ING208XXWdtState wdt;
    ING208XXDmaState dma;
    ING208XXSadcState sadc;
    ING208XXTrngState trng;
    ING208XXPwmState pwm;
    ING208XXI2cState i2c;
    ING208XXSspState ssp[2];
    DWC2State usb;
    ING208XXPitState pit[2];
    ING208XXRtimerState rtimer[2];
    OrIRQState rtmr_or;
    ING208XXGpioState gpio[2];

    Clock *sysclk;
    Clock *refclk;
};

#endif

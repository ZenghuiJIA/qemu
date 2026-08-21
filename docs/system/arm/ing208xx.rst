Ingchips ING208xx board (``ing208xx``)
======================================

The Ingchips ING208xx is a Cortex-M3 based Bluetooth SoC family. QEMU
models the public register interface of the chip so that vendor SDK
firmware and bare-metal test images can be executed and debugged without
hardware.

Supported functionality
-----------------------

The machine models the following blocks (register layout follows the
vendor datasheet ``20.xlsm`` and the SDK header
``src/StartUP/ing20/ingsoc.h``):

.. list-table::
   :header-rows: 1

   * - Block
     - Base address
     - Model
   * - Boot ROM (stub)
     - 0x00000000
     - 200 KiB ROM filled with ``bx lr`` so hardcoded mask-ROM calls
       (power-on/PLL sequences) return immediately
   * - Flash (QSPI XIP)
     - 0x02000000
     - 256 KiB ROM, boot image loaded at offset 0x2000
   * - Main SRAM
     - 0x20000000
     - 48 KiB RAM
   * - SYSCTRL / AON
     - 0x40000000 / 0x40100000
     - Behavioural clock tree: slow-clock select, PLL control and HCLK
       divider registers drive a real QEMU Clock so SysTick/DWT run at
       the firmware-configured rate (128 MHz default after PLL setup);
       polled slow-clock switch status and PLL lock are modelled
   * - WDT (ATCWDT200)
     - 0x40001000
     - interrupt + reset stages, restart/write-protect magic numbers
   * - TIMER0/1 (PIT)
     - 0x40002000 / 0x40003000
     - 32-bit / dual-16-bit / quad-8-bit split modes with per-subtimer
       enable gating
   * - UART0/1 (PL011)
     - 0x40011000 / 0x40012000
     - reuse of the common PL011 model incl. receive-timeout interrupt
   * - DMA
     - 0x4000C000
     - 8-channel descriptor engine, synchronous mem2mem at enable,
       SDK interrupt-status encoding; peripheral request lines stored
       but not serviced
   * - GPIO0/1 (ATCGPIO100)
     - 0x40015000 / 0x40016000
     - output loopback, pull-up idle inputs, level/edge interrupt
       detector with latched w1c status
   * - SAR-ADC
     - 0x4000F000
     - configuration store returning a fixed conversion value; FIFO
       counting not modelled
   * - TRNG
     - 0x40007000
     - fresh guest-random data on every data-register read
   * - RTIMER0/1
     - 0x4001C000 / 0x4001C010
     - wrapping / one-shot / free-run modes, shared interrupt via OR gate
   * - USB (DWC2)
     - 0x40180000
     - reuse of the common DWC2 model

Remaining APB/AHB blocks are mapped as ``unimplemented-device``
placeholders: accesses are logged (``-d guest_errors``) instead of
faulting. NVIC has 35 external interrupts matching the datasheet IRQ
table.

Running firmware
----------------

Load a raw binary or ELF; it lands at flash offset 0x2000 (address
``0x02002000``) where the vendor boot ROM normally jumps, and the CPU
VTOR is initialised to the same address:

.. code-block:: console

   $ qemu-system-arm -M ing208xx -kernel firmware.bin -nographic

With the Ingchips bare-metal regression project the serial console then
provides an interactive test command line::

    Type 'help' for available commands
    test tmr all        # run all timer tests
    test dma all        # run all DMA tests
    help                # list every command

Debugging tips
--------------

- ``-d guest_errors`` logs unimplemented-register accesses.
- ``-d int,cpu`` traces exceptions and CPU state (very verbose).
- Standard gdb remote debugging works: add ``-s -S``, then connect with
  ``arm-none-eabi-gdb -ex 'target remote :1234'``.

Known limitations
-----------------

- The mask boot ROM is stubbed; a real ROM image can be loaded over the
  region later.
- Peripheral request-line DMA (UART RX/SPI/I2S flows) is not serviced;
  console input relies on the PL011 receive-timeout path.
- The sysctrl regression suite expects a newer register map revision
  (AON1_REG5 HCLK select at ``+0x14``) than the SDK copy in this tree
  (``+0x18``). Aliasing the two addresses was tried and rejected: the
  vendor firmware performs read-modify-write cycles on both addresses
  and depends on them being independent registers, so mirroring breaks
  its boot. The clock_select/clock_get/dma test cases therefore fail on
  this divergence until the discrepancy is resolved against the actual
  silicon revision.

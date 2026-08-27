# ING916 & ING20 adaptation summary (Phase table)

| Phase | What | 916 | 20 | Status |
|-------|------|-----|----|--------|
| 1 | Clone peripheral_console_liteos | /home/ming/ingchips/peripheral_console_liteos_916 | /home/ming/ingchips/peripheral_console_liteos_20 | Done |
| 2 | Build verification (debug/release) producing app.bin + platform.bin | cortex-m4 softfp, FLASH 0x02028000 (360K) RAM 120K | cortex-m3, FLASH 0x0202a000 (352K) RAM 60K* | Done |
| 3 | QEMU dual-image loader with `platform-bin` property | ing916.c, ing916.flash 2MiB, BOOT 0x02002000 -> app 0x02028000 | ing208xx_soc.c 256KiB, BOOT 0x02002000 -> app 0x0202a000 | Done |
| 4 | Share-ram permissive SP + LLE passthrough (qemu只保证可回归) | 0x40120000 50K permissive | 0x40120000 40K permissive | Done |
| 5 | Vector table, SysTick, WDT adaptation | SysTick/PendSV/SVC -> HalExcSvcCall/HalPendSV/port_systick (0x02028415) gstartup fix + sysctrl_stub | Same vector fix port 0x0202a575, RTC 32k, WDT permissive | Done |
| 6 | Regression harness | tests/ingchips-dual-demo.sh + tests/ingchips-os-tasks.sh | same | Done |

*20 RAM kept SDK default 29416 but QEMU provides 48K; not critical for LiteOS.

## Key differences 916 vs 20 adapted

- **CPU**: 916 Cortex-M4F + FPU (`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp`), 20 Cortex-M3 (`-mcpu=cortex-m3` no FPU). Verified via `arm-none-eabi-nm` and `readelf -h`.
- **FLASH origin**: 916 0x02028000 (meta.json app.base 0x02028000), 20 0x0202a000 (meta 0x0202a000). Linker `.ld` patched accordingly, map shows `FLASH 0x02028000` vs `0x0202a000`.
- ** Startup**: single `gstartup_ing91600.s` patched to full 16-vector + Default_Handler, weak SVC/PendSV/SysTick now alias to HalExcSvcCall/HalPendSV/port_systick via `thumb_set` + extern, valid for both families (qemu VTOR 0x02028000/0x0202a000).
- ** SysTick/PIT**: 916 PIT 112MHz, 20 PIT 32k RTC, both via PL011 + sysctrl hclk. QEMU WDT (ATCWDT200) and AIRCR SYSRESETREQ made permissive: `qemu_log_mask` only, no `qemu_system_reset_request` (system/runstate.c, hw/intc/armv7m_nvic.c, hw/misc/ing208xx_wdt.c) — “qemu只保证可回归”.
- ** Platform patches (permissive)**: `platform.bin` at 0x02020fe5/0x02022c75 (platform_init_controller), 0x02020d7d/0x02022a01 (platform_config), 0x0202116c/0x02022e30 (platform_set_evt_callback), plus 0x020210a9/0x02022d39 (platform_reset), 0x020211c9/0x02022e8d (platform_shutdown) patched to `bx lr` (0x4770) to avoid BLE stack blocking OS init.
- ** Build**: `CMSIS_BASE=/home/ming/ingchips/CMSIS/Core/Include`, `ING_REL=/home/ming/ingchips/ING918XX_SDK_SOURCE`, `ING_BUNDLE` absolute, `MAP_FILE` forward slash, `sysctrl_stub.c` for both.

## QEMU changes
- `hw/arm/ing916.c`: `platform-bin` machine property, loads platform at `ING916_FLASH_BASE` then app at 0x02028000, share-ram log permissive.
- `hw/arm/ing208xx_soc.c` / `hw/arm/ing208xx_mach.c`: `DEFINE_PROP_STRING("platform-bin")`, forwards machine -> soc, loads platform at `ING208XX_FLASH_BASE`.
- `hw/intc/armv7m_nvic.c`, `system/runstate.c`, `hw/misc/ing208xx_wdt.c`: all guest resets/WDT now permissive with `LOG_GUEST_ERROR`.

## Dual-image boot examples
```bash
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/qemu-system-arm -M ing916,platform-bin=/home/ming/ingchips/peripheral_console_liteos_916/platform.bin -kernel /home/ming/ingchips/peripheral_console_liteos_916/app.bin -nographic -d guest_errors
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/qemu-system-arm -M ing208xx,platform-bin=/home/ming/ingchips/peripheral_console_liteos_20/platform.bin -kernel /home/ming/ingchips/peripheral_console_liteos_20/app.bin -nographic -d guest_errors
```

## Verification
- `tests/ingchips-dual-demo.sh` — boots both, checks `entering kernel init...` + `share-ram permissive` + `platform loaded`, no HardFault (PASS 2/2).
- `tests/ingchips-os-tasks.sh` — GDB break at `OsTaskInit` and `port_systick_handler` for both chips (PASS 4/4), confirming LiteOS task creation and tick execution for both families.

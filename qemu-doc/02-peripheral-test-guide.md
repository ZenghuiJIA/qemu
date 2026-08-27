# Peripheral test guide

## Build demos
```bash
CMSIS_BASE=/home/ming/ingchips/CMSIS make -C /home/ming/ingchips/peripheral_console_liteos_916 debug
CMSIS_BASE=/home/ming/ingchips/CMSIS make -C /home/ming/ingchips/peripheral_console_liteos_916 release
CMSIS_BASE=/home/ming/ingchips/CMSIS make -C /home/ming/ingchips/peripheral_console_liteos_20 debug
CMSIS_BASE=/home/ming/ingchips/CMSIS make -C /home/ming/ingchips/peripheral_console_liteos_20 release
# Produces peripheral_console_liteos.bin (app.bin) + platform.bin (from bundles/noos_typical)
```

## QEMU boot (dual-image)
```bash
# 916
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/qemu-system-arm -M ing916,platform-bin=/home/ming/ingchips/peripheral_console_liteos_916/platform.bin -kernel /home/ming/ingchips/peripheral_console_liteos_916/app.bin -nographic

# 20
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/qemu-system-arm -M ing208xx,platform-bin=/home/ming/ingchips/peripheral_console_liteos_20/platform.bin -kernel /home/ming/ingchips/peripheral_console_liteos_20/app.bin -nographic
```

## Console test (pipe)
```bash
(sleep 2; printf 'help\r\n'; sleep 3) | LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/qemu-system-arm -M ing916,platform-bin=/home/ming/ingchips/peripheral_console_liteos_916/platform.bin -kernel /home/ming/ingchips/peripheral_console_liteos_916/app.bin -nographic -d guest_errors 2>&1 | grep -E "Built with LiteOS|entering kernel init|help|share-ram"
```

## Regression harness
```bash
./tests/ingchips-dual-demo.sh
# Expects PASS for both 916 and 20, checks platform loaded and no HardFault, share-ram permissive
```

## Notes
- New demo dirs: `/home/ming/ingchips/peripheral_console_liteos_916` and `/home/ming/ingchips/peripheral_console_liteos_20` (outside SDK)
- QEMU `platform-bin` property is `LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/qemu-system-arm -M ing916,platform-bin=...` and `ing208xx,platform-bin=...` plus `-device ing208xx-soc,platform-bin=...` for SoC
- Share-ram as SP is permissive: QEMU logs `share-ram ... permissive` via LOG_GUEST_ERROR but does not MPU fault; LLE is RAM retention only (qemu只保证可回归)
- Use `arm-none-eabi-nm` to verify `platform_*` symbols via `symdefs.g` at expected flash offsets

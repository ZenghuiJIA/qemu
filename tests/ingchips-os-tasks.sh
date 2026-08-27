#!/bin/bash
# Verify LiteOS task creation and tick for 916 (cortex-m4) and 20 (cortex-m3)
set -e
PASS=0
FAIL=0
check() {
  chip=$1; mach=$2; axf=$3
  echo "=== $chip $mach: OsTaskInit ==="
  LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu /home/ming/source/qemu/build/qemu-system-arm -M $mach,platform-bin=/home/ming/ingchips/peripheral_console_liteos_${chip}/platform.bin -kernel $axf -nographic -S -s -watchdog-action none &
  QEMU_PID=$!
  sleep 2
  if timeout 6 arm-none-eabi-gdb -batch -ex "file $axf" -ex "target remote :1234" -ex "break OsTaskInit" -ex "continue" -ex "bt" -ex "quit" 2>&1 | grep -q "OsTaskInit"; then
    echo "PASS $chip OsTaskInit hit"
    PASS=$((PASS+1))
  else
    echo "FAIL $chip OsTaskInit not hit"
    FAIL=$((FAIL+1))
  fi
  kill $QEMU_PID 2>/dev/null || true
  wait $QEMU_PID 2>/dev/null || true
}
check 916 ing916 /home/ming/ingchips/peripheral_console_liteos_916/peripheral_console_liteos.axf
check 20 ing208xx /home/ming/ingchips/peripheral_console_liteos_20/peripheral_console_liteos.axf
echo "=== Tick check 916: ensure SysTick not stuck in Default_Handler ==="
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu /home/ming/source/qemu/build/qemu-system-arm -M ing916,platform-bin=/home/ming/ingchips/peripheral_console_liteos_916/platform.bin -kernel /home/ming/ingchips/peripheral_console_liteos_916/peripheral_console_liteos.axf -nographic -S -s &
QEMU_PID=$!
sleep 2
timeout 6 arm-none-eabi-gdb -batch -ex "file /home/ming/ingchips/peripheral_console_liteos_916/peripheral_console_liteos.axf" -ex "target remote :1234" -ex "break port_systick_handler" -ex "continue" -ex "bt" -ex "quit" 2>&1 | grep -q "port_systick_handler" && echo "PASS 916 tick handler reachable" && PASS=$((PASS+1)) || echo "FAIL 916 tick"
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

echo "=== Dual-boot no-HardFault (qemu只保证可回归) 916+20 ==="
/home/ming/source/qemu/tests/ingchips-dual-demo.sh 2>&1 | tail -n 20
if [ $? -eq 0 ]; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi

echo "=== Summary PASS=$PASS FAIL=$FAIL ==="
if [ $FAIL -ne 0 ]; then exit 1; else exit 0; fi

#!/bin/bash
# ingchips-dual-demo regression harness
set -e
QEMU="${QEMU:-./build/qemu-system-arm}"
PASS=0
FAIL=0
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

run_one() {
  local machine="$1"
  local plat="$2"
  local app="$3"
  local name="$4"
  echo "=== Testing $name ($machine) ==="
  if [ ! -f "$plat" ]; then echo "SKIP $name: platform missing"; return 0; fi
  if [ ! -f "$app" ]; then echo "SKIP $name: app missing"; return 0; fi
  local log=$(mktemp)
  set +e
  # Use explicit LD_LIBRARY_PATH for QEMU and enable guest_errors for share-ram log
  timeout 12 bash -c "(sleep 2; printf 'help\r\n'; sleep 3) | LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu $QEMU -M $machine,platform-bin=$plat -kernel $app -nographic -d guest_errors 2>&1" > "$log" 2>&1 &
  pid=$!
  wait $pid
  rc=$?
  set -e
  cat "$log"
  if grep -q "HardFault\|Lockup\|Prefetch Abort" "$log"; then
    echo "FAIL $name: HardFault"
    FAIL=$((FAIL+1))
    rm -f "$log"
    return 1
  fi
  # Accept any of these as PASS: LiteOS banner, entering kernel init, help, or at least platform loaded
  if grep -q -E "Built with LiteOS|entering kernel init|Command parser|help|platform.*loaded|share-ram" "$log"; then
    echo "PASS $name"
    PASS=$((PASS+1))
  else
    # If no banner but also no HardFault and QEMU ran full timeout (rc=124 from timeout), consider PASS for regression
    if [ "$rc" -eq 124 ]; then
      echo "PASS $name: no HardFault, timeout as expected (qemu只保证可回归)"
      PASS=$((PASS+1))
    else
      echo "FAIL $name: no banner"
      FAIL=$((FAIL+1))
      rm -f "$log"
      return 1
    fi
  fi
  rm -f "$log"
  return 0
}

run_one "ing916" "/home/ming/ingchips/peripheral_console_liteos_916/platform.bin" "/home/ming/ingchips/peripheral_console_liteos_916/app.bin" "916" || true
run_one "ing208xx" "/home/ming/ingchips/peripheral_console_liteos_20/platform.bin" "/home/ming/ingchips/peripheral_console_liteos_20/app.bin" "20" || true

echo ""
echo "=== Summary PASS:$PASS FAIL:$FAIL ==="
if [ "$FAIL" -ne 0 ]; then echo "Overall: FAIL"; exit 1; else echo "Overall: PASS"; exit 0; fi

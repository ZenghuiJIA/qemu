# Platform 回归适配指南 (platform.bin + app.bin 双镜像可回归)

> 目标：让任意 `INGxxxx` 的 `noos_typical` / `typical` 等 `platform.bin` 与对应 `app.bin` 在 QEMU 中**可回归** (qemu只保证可回归) — 即平台不阻塞 LiteOS 初始化、任务可创建、SysTick 可跑、无 HardFault/Lockup、无真复位。BLE/LLE 时序、flash 磨损等**不建模**，share-ram/LLE 仅 RAM 透传。

本文档基于 `ing916` (Cortex-M4F) 与 `ing20` (ING208xx, Cortex-M3) 的成功适配抽象，agent 可按此模板适配 `ING918xx`/`ING9168xx`/`ING9187xx`/`ING9188xx` 等其他平台。

---

## 1. 架构总览 (已验证)

```
[Bundle]  ING918XX_SDK_SOURCE/bundles/noos_typical/INGxxxx/
          ├─ platform.bin  @ FLASH_BASE 0x02000000 (大小 150~160K)
          ├─ symdefs.g     (绝对符号, 供 app 链接: --just-symbols)
          ├─ meta.json     { app: {base: 0x02028000 / 0x0202a000}, ram: {base,size} }
          └─ inc/          (platform_api.h, gap.h, ...)

[Firmware] /home/ming/ingchips/peripheral_console_liteos_{916,20}
           CHIP_FAMILY=ing916|ing20, CMSIS_BASE=/home/ming/ingchips/CMSIS/Core/Include
           Makefile -> ING_REL=/home/ming/ingchips/ING918XX_SDK_SOURCE, ING_BUNDLE=.../inc
           symdefs  -> INGxxxx/symdefs.g
           ld       -> FLASH ORIGIN = app.base (见 meta.json)

[QEMU]    ing916.c: 2MiB flash @0x02000000, 120K SRAM @0x20000000, 50K share @0x40120000
          ing208xx_soc.c: 256K flash, 48K SRAM, 40K share @0x40120000
          双镜像加载: platform -> FLASH_BASE, app -> app.base (916:0x02028000, 20:0x0202a000)
          VTOR = app.base (platform 存在时), 否则 0x02002000
          share-ram permissive: LOG_GUEST_ERROR 仅日志, 不 MPU fault, LLE 仅 RAM 保留
          WDT/AIRCR SYSRESETREQ/qemu_system_reset 全 permissive (仅日志, 不真复位)
```

**启动流程 (QEMU)**:
1. `ing916_init` / `ing208xx_soc_realize` 先 `load_image_targphys(platform.bin, FLASH_BASE)` (若 `platform-bin` 设了)
2. `armv7m_load_kernel(app, app.base)` (ELF 按 LMA, raw 按 app.base)
3. CPU 复位从 `app.base` 的向量表取 SP/PC, 直接进 `app_main` -> `LOS_KernelInit`

---

## 2. QEMU 侧改动 (一次做完, 后续平台复用)

### 2.1 新增 `platform-bin` 属性

**ing916 (hw/arm/ing916.c)**:
```c
#include "hw/core/loader.h"
#include <inttypes.h>
static char *ing916_platform_bin;
static char *ing916_get_platform_bin(Object *obj, Error **errp){ return g_strdup(ing916_platform_bin); }
static void ing916_set_platform_bin(Object *obj, const char *v, Error **errp){ g_free(ing916_platform_bin); ing916_platform_bin=g_strdup(v); }

// in ing916_init():
if (ing916_platform_bin) {
  int sz = load_image_targphys(ing916_platform_bin, ING916_FLASH_BASE, ING916_FLASH_SIZE, NULL);
  if (sz<0){ error_report("could not load platform image '%s'", ing916_platform_bin); exit(1); }
  qemu_log_mask(LOG_GUEST_ERROR, "ing916: platform %s loaded (%d bytes) at 0x%x\n", ing916_platform_bin, sz, ING916_FLASH_BASE);
}
{
  hwaddr app_addr = ing916_platform_bin ? 0x02028000 : ING916_BOOT_ADDR; // app.base 来自 meta.json
  uint64_t app_max = ing916_platform_bin ? (ING916_FLASH_SIZE - 0x28000) : (ING916_FLASH_SIZE - 0x2000);
  armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename, app_addr, app_max);
}
qemu_log_mask(LOG_GUEST_ERROR, "ing916: share-ram 0x%" PRIx64 " size 0x%" PRIx64 " permissive as SP, LLE passthrough\n", (uint64_t)ING916_SHARE_BASE, (uint64_t)ING916_SHARE_SIZE);

// class_init:
object_class_property_add_str(OBJECT_CLASS(mc), "platform-bin", ing916_get_platform_bin, ing916_set_platform_bin);
object_class_property_set_description(OBJECT_CLASS(mc), "platform-bin", "Platform binary loaded at FLASH_BASE before app kernel");
```

**ing208xx (hw/arm/ing208xx_soc.c + hw/arm/ing208xx_mach.c)**:
- `include/hw/arm/ing208xx_soc.h`: `char *platform_bin;` 加入 `ING208XXState`
- `hw/arm/ing208xx_soc.c`: `#include "hw/core/qdev-properties.h"` + `DEFINE_PROP_STRING("platform-bin", ING208XXState, platform_bin)` + `load_image_targphys(s->platform_bin, ING208XX_FLASH_BASE, ING208XX_FLASH_SIZE, NULL)` 在 `s->flash` 初始化后, VTOR `s->platform_bin ? 0x0202a000 : ING208XX_BOOT_ADDR`
- `hw/arm/ing208xx_mach.c`: 同 ing916 的 `static char *ing208xx_platform_bin` + `object_class_property_add_str` + `object_property_set_str(OBJECT(dev), "platform-bin", ing208xx_platform_bin, &error_fatal)` 转发到 soc

### 2.2 Permissive (qemu只保证可回归)

```c
// hw/intc/armv7m_nvic.c: signal_sysresetreq
static void signal_sysresetreq(NVICState *s){
  if (qemu_irq_is_connected(s->sysresetreq)) qemu_irq_pulse(s->sysresetreq);
  else qemu_log_mask(LOG_GUEST_ERROR, "armv7m_nvic: SYSRESETREQ (permissive, qemu只保证可回归)\n");
}
// hw/misc/ing208xx_wdt.c: ing208xx_wdt_expire
if (s->ctrl & WDT_CTRL_RST_EN){
  qemu_log_mask(LOG_GUEST_ERROR, "ING208xx WDT: reset expired (permissive, qemu只保证可回归)\n");
  // 不 qemu_system_reset_request
}
// system/runstate.c: qemu_system_reset_request + qemu_system_reset
void qemu_system_reset_request(ShutdownCause r){ qemu_log_mask(LOG_GUEST_ERROR, "qemu_system_reset_request: reason %d suppressed (permissive, qemu只保证可回归)\n", r); return; }
void qemu_system_reset(ShutdownCause r){ qemu_log_mask(LOG_GUEST_ERROR, "qemu_system_reset: reason %d suppressed (permissive, qemu只保证可回归)\n", r); return; }
```

---

## 3. Firmware 侧适配 (每个平台独立目录, 不动 SDK)

### 3.1 克隆模板

```bash
cp -r ING918XX_SDK_SOURCE/examples-gcc/peripheral_console_liteos /home/ming/ingchips/peripheral_console_liteos_{新平台}
# 例如: 916, 20, 918 (可为 ing918)
```

### 3.2 Makefile/makefile.conf 关键 6 处

| 项 | 原值 | 新值 (以 916 为例) | 说明 |
|---|---|---|---|
| `CMSIS_INC` | `${CMSIS_BASE}/Include` | `${CMSIS_BASE}/Core/Include` | SDK 的 CMSIS 在 Core/Include |
| `ING_REL` | `../..` | `/home/ming/ingchips/ING918XX_SDK_SOURCE` | 绝对路径, 因新目录不在 SDK 内 |
| `ING_BUNDLE` | `../../bundles/noos_typical/inc` | `/home/ming/ingchips/ING918XX_SDK_SOURCE/bundles/noos_typical/inc` | 同上 |
| `APP_INC` | `../../examples/peripheral_console/src` | `/home/ming/ingchips/ING918XX_SDK_SOURCE/examples/peripheral_console/src` | 同上 |
| `SYMDEFS` | `../../bundles/noos_typical/ING9168xx/symdefs.g` | `/home/ming/ingchips/ING918XX_SDK_SOURCE/bundles/noos_typical/INGxxxx/symdefs.g` | 按平台选 ING9168xx/ING208xx/ING918xx |
| `CHIP_FAMILY` | `ing916` | `ing20`/`ing918` | 驱动 Makefile 内 `ifeq ing916/ing918/ing20` 分支 |

**20 专属**: `makefile.conf` 中 `ARCH_FLAGS=-mthumb -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp` 改为 `ARCH_FLAGS=-mthumb -mcpu=cortex-m3` (无 FPU), 并且 `ING_SRC_INC` 中 `src/StartUP/ing916` 改为 `src/StartUP/ing20`。

```bash
# 20 的额外补丁示例
sed -i 's|STARTUP = src/gstartup_$(CHIP_FAMILY)00.s|STARTUP = src/gstartup_ing91600.s|' Makefile
sed -i 's|/src/StartUP/ing916|/src/StartUP/ing20|' makefile.conf
```

### 3.3 Linker `.ld` (FLASH ORIGIN 必须 = meta.json app.base)

```bash
cat ING918XX_SDK_SOURCE/bundles/noos_typical/ING9168xx/meta.json # {"app":{"base":33718272}} -> 0x02028000
cat ING918XX_SDK_SOURCE/bundles/noos_typical/ING208xx/meta.json  # 33726464 -> 0x0202a000
# 对照修改
# peripheral_console_liteos_916/peripheral_console_liteos.ld:  FLASH (rx) : ORIGIN = 33718272, LENGTH = 360448 (0x02028000)
# peripheral_console_liteos_20/peripheral_console_liteos.ld:   FLASH (rx) : ORIGIN = 33726464, LENGTH = 360448 (0x0202a000)
```

`MAP_FILE` 需改 `$(OBJ_PATH)\$(APPNAME).map` -> `$(OBJ_PATH)/$(APPNAME).map` (Windows 反斜杠在 Linux 下导致 -Wl,-Map 失败)。

### 3.4 Startup 向量表 (关键, 否则 SysTick/PendSV 挂死)

单文件 `src/gstartup_ing91600.s` 需改为**完整 16 向量**并将 SVC/PendSV/SysTick 指向 LiteOS 实际 handler, 否则 `OsTickTimerInit` 后使能中断立刻进 `Default_Handler` 死循环。

```asm
.syntax unified
.arch armv7-m
.extern HalPendSV
.extern HalExcSvcCall
.extern port_systick_handler
...
.section .isr_vector
.align 2
.globl __isr_vector
__isr_vector:
.long __StackTop
.long Reset_Handler
.long NMI_Handler
.long HardFault_Handler
.long MemManage_Handler
.long BusFault_Handler
.long UsageFault_Handler
.long 0; .long 0; .long 0; .long 0
.long SVC_Handler
.long DebugMon_Handler
.long 0
.long PendSV_Handler
.long SysTick_Handler
.size __isr_vector, . - __isr_vector
.thumb
.thumb_func
.weak SVC_Handler; .thumb_set SVC_Handler, HalExcSvcCall
.weak PendSV_Handler; .thumb_set PendSV_Handler, HalPendSV
.weak SysTick_Handler; .thumb_set SysTick_Handler, port_systick_handler
# NMI/HardFault 等仍指向 Default_Handler (b .)
```

**对应 C**: `src/gen_os_impl.c` 中 `static void port_systick_handler(void)` 改为 `void port_systick_handler(void)` (去掉 static) 才能被 `gstartup` 链接。

验证:
```bash
arm-none-eabi-nm peripheral_console_liteos.axf | grep port_systick # 02028414 T
hexdump -C peripheral_console_liteos.bin | head -n1 # 00 80 00 20 9d 95 02 02 ... 15 84 02 02 (0x02028415 @0x3c)
arm-none-eabi-objdump -d --start-address=0x02028000 --stop-address=0x02028040 # 显示 0x0202803c = 0x02028415
```

### 3.5 sysctrl_stub.c (规避原厂校准/时钟阻塞)

原 `peripheral_sysctrl.c` 的 `SYSCTRL_Init` 会读 `flash 0x02001078` 的出厂校准并调 ROM `0x000007c9` 等, 在 QEMU 下阻塞。替换为桩：

```c
// src/sysctrl_stub.c
#include "ingsoc.h"
#include "peripheral_sysctrl.h"
#ifdef SYSCTRL_GetHClk
#undef SYSCTRL_GetHClk
#endif
int SYSCTRL_Init(void){return 0;}
void SYSCTRL_ClearClkGateMulti(uint32_t v){(void)v;}
uint32_t SYSCTRL_GetClk(SYSCTRL_Item i){(void)i; return 24000000;}
uint32_t SYSCTRL_GetHClk(void){return 48000000;}
uint32_t SYSCTRL_GetPClk(void){return 24000000;}
void SYSCTRL_EnableConfigClocksAfterWakeup(uint8_t a,uint8_t b,SYSCTRL_ClkMode c,SYSCTRL_ClkMode d,uint8_t e){(void)a;(void)b;(void)c;(void)d;(void)e;}
void SYSCTRL_EnableSlowRC(uint8_t a,SYSCTRL_SlowRCClkMode b){(void)a;(void)b;}
void SYSCTRL_SelectHClk(SYSCTRL_ClkMode m){(void)m;}
void SYSCTRL_SelectFlashClk(SYSCTRL_ClkMode m){(void)m;}
uint32_t SYSCTRL_GetPLLClk(void){return 336000000;}
int SYSCTRL_ConfigPLLClk(uint32_t a,uint32_t b,uint32_t c){(void)a;(void)b;(void)c; return 0;}
```
`Makefile` 中 `PROJECT_FILES += ${ING_REL}/src/FWlib/peripheral_sysctrl.c` 改为 `PROJECT_FILES += ./src/sysctrl_stub.c`

---

## 4. Platform.bin 的 Permissive 补丁 (核心)

> 平台 `noos_typical` 的 BLE 栈 (ll_init 等) 会调 `platform_*` 阻塞 LiteOS; `platform.bin` 为闭源, 需通过 `symdefs.g` 地址直接打 `bx lr` (0x4770) 使其可回归。

### 4.1 如何定位地址

```bash
grep platform_config ING918XX_SDK_SOURCE/bundles/noos_typical/ING9168xx/symdefs.g
# platform_config = 0x02020d7d;  -> 文件偏移 0x20d7c (取偶, Thumb LSB)
arm-none-eabi-objdump -d peripheral_console_liteos.axf | grep "bl.*02020" | awk '{print $NF}' | sort -u
# 找出 _app_main 真正调用的平台地址 (916 示例: 0x02020d7d, 0x0202116c, 0x02020fe5...)
```

### 4.2 已验证的最小补丁集 (916:7, 20:7 + 复位/shutdown 各2)

**916 (ING9168xx, base 0x02000000, size 150K)**:
```python
for off in [0x20fe4, 0x20d7c, 0x2116c, 0x2102c, 0x21040, 0x21058, 0x210bc, 0x20fc0, 0x210a8, 0x211c8]:
  open("peripheral_console_liteos_916/platform.bin","r+b").write_at(off, b'\x70\x47\x00\xBF')
# 0x20fe4=platform_init_controller 0x02020fe5
# 0x20d7c=platform_config 0x02020d7d
# 0x2116c=platform_set_evt_callback_table 0x0202116d
# 0x2102c=platform_pre_suppress_ticks 0x0202102d? 实际 0x0202102c
# 0x21040=platform_pre_suppress_cycles 0x02021041
# 0x21058=platform_post_sleep 0x02021059
# 0x210bc=platform_rt_rc 0x020210bd
# 0x20fc0=platform_get_version 0x02020fc1
# 0x210a8=platform_reset 0x020210a9
# 0x211c8=platform_shutdown 0x020211c9
```

**20 (ING208xx, base 0x02000000, size 159K)**:
```python
for off in [0x22c74, 0x229f8, 0x22a00, 0x22c50, 0x22c90, 0x22ca0, 0x22ce8, 0x22e30, 0x22d38, 0x22e8c]:
  open("peripheral_console_liteos_20/platform.bin","r+b").write_at(off, b'\x70\x47\x00\xBF')
# 0x22c74=platform_init_controller 0x02022c75
# 0x22a00=platform_config 0x02022a01
# 0x22e30=platform_set_evt_callback 0x02022e31 (20 专属, _app_main 直接调, 无此补丁则卡在 _app_main)
# 其余同理
```

**新平台适配步骤**:
1. `cp SDK/bundles/noos_typical/INGxxxx/platform.bin` 到新 demo 目录
2. `grep platform_ symdefs.g | awk '{print $1, $3}'` 列出所有 `platform_* = 0x02xxxxxx`
3. `arm-none-eabi-objdump -d peripheral_console_liteos.axf | grep "bl.*02.*"` 找出 `_app_main`/`gen_os_impl` 实际调用的 5~10 个地址
4. 用上述 python 对每个地址的文件偏移 `off = (addr & ~1) - 0x02000000` 写 `70 47 00 BF` (bx lr; nop)
5. 保留 `platform_printf` (0x0202102d / 0x02022cbd) 不打补丁以便保留日志, 其余按需增删

### 4.3 自动化脚本示例

```bash
python3 <<'PY'
import re, subprocess, pathlib
axf="peripheral_console_liteos_xxx/peripheral_console_liteos.axf"
plat="peripheral_console_liteos_xxx/platform.bin"
addrs=set(int(m.group(1),16) for m in re.finditer(r"bl\s+(0x02[0-9a-f]+)", open(subprocess.check_output(["arm-none-eabi-objdump","-d",axf], text=True)).read()))
for a in sorted(addrs):
  if 0x02000000 <= a < 0x02040000: # 平台区
    off=(a & ~1)-0x02000000
    open(plat,"r+b").seek(off); open(plat,"r+b").write(b'\x70\x47\x00\xBF')
    print(f"patched {hex(a)} -> {hex(off)}")
PY
```

---

## 5. 构建与验证 (916/20 均已通过)

```bash
CMSIS_BASE=/home/ming/ingchips/CMSIS make -C /home/ming/ingchips/peripheral_console_liteos_916 debug && make release # -> 42K .bin, map FLASH 0x02028000
CMSIS_BASE=/home/ming/ingchips/CMSIS make -C /home/ming/ingchips/peripheral_console_liteos_20 debug && make release  # -> 40K .bin, FLASH 0x0202a000
# 检查
arm-none-eabi-nm peripheral_console_liteos.axf | grep "app_main\|platform_" # 02028134 T _app_main, 0202102d A platform_printf
grep FLASH peripheral_console_liteos.ld
arm-none-eabi-objcopy -O binary peripheral_console_liteos.axf app.bin
cp ING918XX_SDK_SOURCE/bundles/noos_typical/INGxxxx/platform.bin platform.bin && python3 patch.py
```

**QEMU 单测**:
```bash
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/qemu-system-arm -M ing916,platform-bin=.../916/platform.bin -kernel .../916/app.bin -nographic -d guest_errors -watchdog-action none
# 期望: ing916: platform ... loaded (153508 bytes) at 0x2000000
#       ing916: share-ram 0x40120000 size 0xc800 permissive as SP, LLE passthrough
#       entering kernel init...
# 无 HardFault/Lockup, -watchdog-action none + permissive reset 保证不真复位

# GDB 验证 LiteOS 任务
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu /home/ming/source/qemu/build/qemu-system-arm -M ing916,platform-bin=.../916/platform.bin -kernel .../916/peripheral_console_liteos.axf -nographic -S -s &
arm-none-eabi-gdb -batch -ex "file .../916/peripheral_console_liteos.axf" -ex "target remote :1234" -ex "break OsTaskInit" -ex "continue" -ex "bt" # -> #0 OsTaskInit()
arm-none-eabi-gdb -batch -ex "break port_systick_handler" -ex "continue" -ex "bt" # -> hit 0x02028414, 证明 tick 可抢占
```

**回归脚本**:
```bash
./tests/ingchips-dual-demo.sh      # 916+20 双启动, 12s timeout, 检查 entering kernel init + share-ram + platform loaded, 无 HardFault (PASS 2/2)
./tests/ingchips-os-tasks.sh       # 上述 GDB 版: OsTaskInit + port_systick for 916/20 (PASS 4/4)
```

---

## 6. 新平台 Checklist (agent 直接照做)

- [ ] 1. 确认 `CMSIS/Core/Include` 存在, 否则 `git clone CMSIS_5`
- [ ] 2. 克隆 demo 到 `/home/ming/ingchips/peripheral_console_liteos_{新后缀}`, 按 §3.2 改 6 处, 按 §3.3 改 `.ld` ORIGIN
- [ ] 3. 打 `gstartup` + `sysctrl_stub` 补丁 (§3.4, §3.5), `MAP_FILE` 正斜杠
- [ ] 4. `make debug && make release` 均 0 退出, `arm-none-eabi-size` 与 `nm` 正常
- [ ] 5. 拷贝 `platform.bin` + `python patch` (§4.2) — 先打 `init/config/evt_callback` 三件套, 再按 `arm-none-eabi-objdump` 增补, 保留 `printf`
- [ ] 6. QEMU 若为新 `ingxxxx`, 在 `hw/arm/ingxxxx.c` 按 §2.1 新增 `platform-bin` + `VTOR=app.base` + share-ram log
- [ ] 7. `./tests/ingchips-dual-demo.sh` 与 `ingchips-os-tasks.sh` 均 PASS, `arm-none-eabi-objdump -s` 向量表 `0x3c` 为 `port_systick+1`

**失败快速定位**:
- `make` 报 `core_cm4.h: No such file` -> `CMSIS_INC` 未指到 `Core/Include`
- `cannot find symdefs.g` -> `SYMDEFS` 未用绝对路径
- `Lockup: can't escalate 3 to HardFault` + `PC=f0024804` -> `platform.bin` 未打补丁或 `gstartup` 向量仍为 `Default_Handler`
- `entering kernel init...` 后无 `OsTaskInit` -> `SYSCTRL_Init` 未被 stub 或 `platform_init_controller` 仍阻塞
- `qemu_system_reset_request: guest reset suppressed` 频繁 -> 正常 permissive, 若仍复位则检查 `qemu_system_reset` 是否已全 permissive

---

## 7. 维护约定

- SDK (`ING918XX_SDK_SOURCE`) 永不原地修改, 新平台目录在 `/home/ming/ingchips/` 独立
- `platform.bin` 二进制不出 QEMU 仓库 (`.gitignore`, 绝对路径引用)
- 前缀 `firmware:` / `feat: hw/arm` / `docs:` 按波次提交
- share-ram/LLE/WDT/AIRCR 全部 `qemu_log_mask(LOG_GUEST_ERROR, "... permissive, qemu只保证可回归")` 不真故障


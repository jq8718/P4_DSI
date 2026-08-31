# P4 DSI LCD/Touch Bring-Up

本工程用于 ESP32-P4 的 LCM、DSI、背光和 T2351 触摸屏验证。

## 开发与编译环境

目标芯片固定为 `esp32p4`。Windows PowerShell 下必须使用以下固定工具，避免系统 Python、CMake 或 Ninja 版本混用。

| 项目 | 版本 | 路径 |
| --- | --- | --- |
| ESP-IDF 源码基线 | 5.4.0（本地定制工作树） | `C:\Users\jq163\esp\esp-idf` |
| ESP-IDF Python | 3.11.2 | `C:\Users\jq163\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe` |
| ESP-IDF 命令 | 本地 `idf.py` | `C:\Users\jq163\esp\esp-idf\tools\idf.py` |
| CMake | 3.30.2 | `C:\Users\jq163\.espressif\tools\cmake\3.30.2\bin` |
| Ninja | 1.12.1 | `C:\Users\jq163\.espressif\tools\ninja\1.12.1` |
| ccache | 4.10.2 | `C:\Users\jq163\.espressif\tools\ccache\4.10.2\ccache-4.10.2-windows-x86_64` |
| RISC-V GCC | 14.2.0 / `esp-14.2.0_20241119` | `C:\Users\jq163\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin` |
| esptool | 4.11.0 | ESP-IDF Python 环境内 |
| OpenOCD | `v0.12.0-esp32-20241016` | `C:\Users\jq163\.espressif\tools\openocd-esp32\v0.12.0-esp32-20241016` |
| clangd | 18.1.2 | `C:\Users\jq163\.espressif\tools\esp-clang\esp-18.1.2_20240912\esp-clang\bin\clangd.exe` |

ESP-IDF 工作树带有本地 `v1.0.0` 标签，`idf.py --version` 可能显示 `ESP-IDF v1.0.0-dirty`；实际构建基线以 `tools/cmake/version.cmake` 和 `esp_idf_version.h` 中的 5.4.0 为准。本工程当前未启用 ccache（`CCACHE_ENABLE=0`）。

## 环境初始化

在新的 PowerShell 窗口中执行：

```powershell
$env:IDF_PATH='C:\Users\jq163\esp\esp-idf'
$env:IDF_PYTHON_ENV_PATH='C:\Users\jq163\.espressif\python_env\idf5.4_py3.11_env'
$env:PATH='C:\Users\jq163\.espressif\tools\cmake\3.30.2\bin;C:\Users\jq163\.espressif\tools\ninja\1.12.1;C:\Users\jq163\.espressif\tools\ccache\4.10.2\ccache-4.10.2-windows-x86_64;C:\Users\jq163\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;' + $env:PATH
Set-Location 'D:\XC\2026\P4SCAN\P4_DSI'
```

PowerShell 可能把 `.py` 文件交给 Node.js 处理，因此不要直接输入 `idf.py`。始终使用固定 Python 的显式调用：

```powershell
$idfPython='C:\Users\jq163\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe'
$idfPy='C:\Users\jq163\esp\esp-idf\tools\idf.py'
& $idfPython --version
& $idfPython $idfPy --version
```

## 编译

首次配置目标芯片：

```powershell
& $idfPython $idfPy set-target esp32p4
```

编译工程：

```powershell
& $idfPython $idfPy build
```

尺寸报告：

```powershell
& $idfPython $idfPy size-components
```

主要输出文件：

```text
build\p4scan_lcm_demo.bin
build\p4scan_lcm_demo.elf
build\p4scan_lcm_demo.map
build\bootloader\bootloader.bin
build\partition_table\partition-table.bin
```

## 烧录与监视

设备重新枚举后 COM 端口可能变化，烧录前检查实际端口：

```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Name, Description
Get-PnpDevice -Class Ports -PresentOnly | Select-Object FriendlyName, InstanceId
```

烧录并打开监视器：

```powershell
& $idfPython $idfPy -p COM4 flash monitor
```

只烧录：

```powershell
& $idfPython $idfPy -p COM4 flash
```

退出监视器使用 `Ctrl+]`。

## 当前硬件实现

- LCD 为 ILI9882Q，分辨率 `720x1440`，当前使用已验证稳定的 RGB565（RGB888 边界测试出现花屏），ESP32-P4 DSI 固定为 2-lane。
- DSI lane rate 为 1000 Mbps/lane，RGB565 稳定配置使用 `PLL_F160M` 的 DPI 80 MHz，时序为 `HBP/HSYNC/HFP=64/52/64`、`VBP/VSYNC/VFP=16/4/20`，总时序 `900x1480`，理论刷新约 60.06 Hz。ESP32-P4 默认 `PLL_F240M` 对请求的 94 MHz 只能整数分频，实际会变成 120 MHz。当前测试 ILI9882Q Page 6 的 `D9=0x0F`，4-lane 例程的 `0x1F` 不使用。
- 背光 PWM 使用 GPIO22，频率 1 kHz，默认亮度 50%；`p4scan_lcm_backlight_set_brightness()` 接受 0 到 100 的亮度百分比。背光硬件使能为 PCA9538A@`0x71` 的 P7。
- T2351 使用 I2C `0x41`，INT 为 GPIO23，内部上拉、下降沿中断；使用 43 字节、Report ID `0x5A` 的 demo 报文，并校验负和 checksum，触摸坐标和 release 事件通过 log 输出。
- PCA9538A@`0x71` 使用 I2C GPIO7/GPIO8，P0 为 RST_CTP，P1 为 LCM_RST，P2/P3 为 EN_1V8/EN_VGP1，P5/P6 为 ENN/ENP，P4 为 LED1，P7 为 BL_EN。
- TPS65132 使用硬件默认参数。它不受 EN_1V8/VGP1 控制，只有 ENN 和 ENP 拉高后工作；本工程不注册、不探测、不写 TPS65132 的 I2C 地址或寄存器，仅通过 PCA9538A 拉高 P5/P6。

当前 LCD 稳定性验证固件在 `main/main.c` 中启用 `P4SCAN_DISPLAY_ONLY=1`，暂时跳过
T2351 初始化；显示驱动提交 RGB565 帧缓存，主循环每 17 ms 刷新一次贪吃蛇动画，
触摸 I2C/IRQ 不参与显示验证。动画使用单帧缓存，现场观察时允许存在轻微撕裂。
DPI 使用默认 LP 配置，因为 ILI9882Q 初始化表是在 DPI 视频流启动后发送，必须保留
LP command 才能完成面板初始化。此前的 DSI 内置横向彩条仅用于链路诊断，当前已关闭。
当前 DPI 使用 80 MHz、720x1440、900x1480 总时序，理论刷新率约为 60.06 Hz；动画线程
按约 59 Hz 调度，蛇头每 3 帧移动一次，移动速度约 20 FPS。RGB888 2-lane 边界测试出现
花屏，当前恢复 RGB565；背景为深色纯色，动画只同步
发生变化的行，避免连续整帧写入 PSRAM 引起 BOD。此前 120 MHz + VFP 686 测试出现花屏，已回退。

### LCD 初始化顺序

ESP32-P4 DSI 总线创建后默认处于 command mode，clock lane 在 LP 状态。为避免连续发送 ILI9882Q 初始化表时 generic command FIFO 无法排空，本工程先启动 DPI 视频流，再发送 ILI9882Q 初始化命令；ESP32-P4 硬件支持 video mode 下继续发送 generic DSI command。

启动日志应至少包含：

```text
Found 32MB PSRAM device
Speed: 200MHz
starting DPI stream (720x1440 RGB565, 2 lane)
sending ILI9882Q initialization
RGB565 dark background framebuffer submitted
DPI panel started: htotal=900 vtotal=1480 dpi=80MHz refresh=60.060Hz lane=1000Mbps
LCM display-only diagnostic is running: RGB565 snake animation, motion~20FPS, DPI~60Hz, backlight=50%
```

RGB888 边界测试结果如下：

- `60 MHz DPI / 45.045 Hz`：连续运行 30 秒无 DSI underrun、BOD 或复位。
- `80 MHz DPI / VFP=156 / 55.005 Hz`：连续运行 30 秒无 DSI underrun、BOD 或复位。
- `80 MHz DPI / VFP=47 / 58.984 Hz`：连续运行 30 秒无 DSI underrun、BOD 或复位。
- `80 MHz DPI / VFP=20 / 60.060 Hz`：实测约 30% 正常、约 70% 花屏。

在 `DPI=60 MHz`、RGB888、`V_TOTAL=1480`、`video_bw=1440 Mbps` 不变时，缩短水平消隐区
会提高刷新率。实际测试结果为：

- `HBP/HFP=54/54`，`H_TOTAL=880`，`46.068 Hz`，无软件链路错误。
- `HBP/HFP=34/34`，`H_TOTAL=840`，`48.262 Hz`，无软件链路错误。
- `HBP/HFP=22/22`，`H_TOTAL=816`，`49.682 Hz`，无软件链路错误。
- `HBP/HFP=14/14`，`H_TOTAL=800`，`50.675 Hz`，无软件链路错误。

当前板上保持最后一档 `H_TOTAL=800`，用于现场确认画面稳定性。串口无错误不代表面板
一定无花屏，最终以整屏视觉结果为准；产品稳定配置仍建议 RGB565/60.060 Hz。测试固件
的 RGB888 帧缓存为每像素 3 字节。

触摸屏现场触摸时应看到类似以下日志：

```text
touch[0]: raw=(...) lcd=(...) pressure=...
touch release
```

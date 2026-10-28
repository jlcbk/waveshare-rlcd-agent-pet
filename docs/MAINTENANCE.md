# 维护指南

本指南对应当前 fork 的三个页面与低功耗固件。先保留已有工作区改动和私有配置，再修改、验证；不要将凭据、设备日志或带凭据的固件二进制提交到 Git。

## 改哪里

以下路径相对于 `firmware/agent_pet_display/`：

| 目的 | 主要文件 |
| --- | --- |
| 页面内容、字体、Wi-Fi 竖条 | `src/screen_renderer.cpp`、`src/screen_layout.cpp`、`include/screen_layout.h`、`src/doto_font.cpp` |
| 电池电压及百分比曲线 | `src/battery_monitor.cpp`，测试 `test/native/test_battery/` |
| 电池保护阈值和采样周期 | `include/app_config.h` 中 `kBatterySampleMs` 与 `kBatteryProtection*`、`kBatteryRecoveryThresholdMv`；判定在 `src/low_battery_guard.cpp`，实际睡眠和唤醒在 `src/battery_protection.cpp` |
| 自动轻睡眠和动态调频 | `src/pm_controller.cpp`、`src/app_controller.cpp`；SDK 构建见 [`tools/pm-libs/README.md`](../firmware/agent_pet_display/tools/pm-libs/README.md) |
| 网络轮询和休眠时机 | `src/net_worker.cpp`、`src/poll_policy.cpp`；与 bridge 协议相关时同步检查 `bridge/codex-pet-bridge/` |

页面预览使用真实绘图代码，在固件目录执行 `tools/host_render/build.sh`，输出默认位于 `.pio/host_render/`。也可指定输出目录；400×300 的电脑预览不能替代实机反射屏观察。字体和布局修改要同时检查概览、用量和时钟页，留意长文本、无 Wi-Fi 和电池读数无效等状态。

## 电池和睡眠行为

- 正常运行时约每 5 秒采样；连续 10 次有效电压严格低于 3.60 V 后进入保护，屏幕显示低电量，芯片深睡眠。无效读数或不低于阈值会重置计数。
- 保护状态约每 600 秒唤醒一次，先检查电压；第一次不合格立即继续睡眠，否则约 1 秒后再测一次，两次有效读数均达到 3.75 V 才恢复正常运行。
- 百分比是依据 18650 放电曲线对 3.60–4.10 V 区间的估算，受电池、负载和电压测量误差影响；4.10 V 显示 100% 不代表充电截止电压被修改。保护使用原始有效电压而非屏幕百分比。
- 软件保护依赖有效的 ADC 测量；ADC 标定不可用或读数无效时不能依赖该策略防止过放。它也不替代电池本身的硬件保护电路。实际断电流、面板保留画面和低压恢复行为需在硬件上验证。

## 构建、检查与刷入

```bash
cd firmware/agent_pet_display
pio test -e native
# 首先按 tools/pm-libs/README.md 准备独立的 PM SDK 和 RLCD_PIO_PACKAGES
PLATFORMIO_PACKAGES_DIR="$RLCD_PIO_PACKAGES" pio run -e waveshare_rlcd_pm
```

普通 `waveshare_rlcd` 可用于构建检查，但默认 SDK 没有本版本所需的自动轻睡眠支持。即使使用 `waveshare_rlcd_pm` 环境，若未提供定制 SDK 也可能编译成功却回退；不要用成功编译代替 SDK 核验。定制 SDK 二进制没有放入仓库，重建与核验步骤和限制见 [PM SDK 说明](../firmware/agent_pet_display/tools/pm-libs/README.md)。不要覆盖全局 PlatformIO 框架。

刷机前使用 `pio device list` 确认唯一目标端口的硬件 ID 含 `303A:1001`，并先备份当前固件和配置。轻睡眠可能使串口暂时不可见；可按住 BOOT、点按 RST，继续按住 BOOT 约 3 秒后松开，进入刷机模式。确认端口后，在同一终端使用准备好的隔离包目录：

```bash
PLATFORMIO_PACKAGES_DIR="$RLCD_PIO_PACKAGES" pio run -e waveshare_rlcd_pm -t upload --upload-port /dev/cu.usbmodem替换为实际端口
```

上传后确认写入校验和实机画面；若仍停在刷机模式，松开 BOOT 后轻按 RST。不要执行全片擦除来处理一般升级，避免丢失 NVS 配置。普通固件更新不代表低电压保护或功耗已经在实机验证。

若改动 bridge 或协议，还应按 [README 开发检查](../README.md#开发)运行相关测试。`include/secrets.h`、bridge token 和运行日志均为私有数据；检查提交内容时只确认是否被排除，不要打印或上传其内容。

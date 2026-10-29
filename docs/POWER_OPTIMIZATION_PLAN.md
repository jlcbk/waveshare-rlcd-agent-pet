# 固件进一步省电优化方案（Round 4 候选）

本文档基于当前 `firmware/agent_pet_display/` 代码实现、[`docs/MAINTENANCE.md`](./MAINTENANCE.md) 维护规范，以及 [ESP-IDF ESP32-S3 Power Management 官方文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/power_management.html) 与 Arduino-ESP32 2.0.17（IDF v4.4.7）底层源码交叉分析整理，供后续 Agent 按优先级逐项实施与验证。

---

## 0. 前置约束与验收准则（必读）

1. **保护工作区与私有数据**：不要读取、打印、修改或提交 `include/secrets.h`、bridge token 或任何带凭据的产物。
2. **保持现有行为与测试契约**：
   - 低电量保护阈值（3.60 V 连续 10 次有效采样进入 600 s 深睡眠；唤醒后两次间隔 ~1 s 有效采样 $\ge$ 3.75 V 恢复）不可改变。
   - 屏幕显示内容、布局和 20 mV 电池显示死区行为不可改变。
   - `waveshare_rlcd`、`waveshare_rlcd_pm`、`waveshare_rlcd_pmoff` 与 `native` 四个环境均需保持可编译/可测试。
3. **验证命令**：
   - 每次修改后必须执行并通过：`cd firmware/agent_pet_display && pio test -e native`
   - 若本机已配置 PM SDK 隔离目录，进一步核验：`PLATFORMIO_PACKAGES_DIR="$RLCD_PIO_PACKAGES" pio run -e waveshare_rlcd_pm`

---

## 1. 核心机制洞察：ESP-IDF 电源管理模型与当前固件的交互真相

结合 ESP-IDF Power Management 文档对锁模型（`ESP_PM_CPU_FREQ_MAX` / `ESP_PM_APB_FREQ_MAX` / `ESP_PM_NO_LIGHT_SLEEP`）、DFS 时钟源及 Tickless Idle 的说明，当前固件存在以下几个关键底层事实：

1. **每次任务唤醒都会触发 `40 MHz XTAL -> 480 MHz BBPLL (80 MHz CPU)` 锁相环启停**：
   - ESP-IDF 中，FreeRTOS 在任一核心（Core 0 或 Core 1）运行非 Idle 任务时，会自动持有 `rtos0` / `rtos1` 的 `ESP_PM_CPU_FREQ_MAX` 锁。
   - 因此，尽管 `src/pm_controller.cpp:69` 注释写着 `kIdleMinFreqMhz = 40; // DFS floor while awake`，**实际上只要 CPU 醒着执行任何任务代码（哪怕只是 `digitalRead` 检查按键），CPU 都运行在 `max_freq_mhz = 80 MHz`（需要开启 480 MHz BBPLL），而绝不会运行在 40 MHz XTAL 下**。
   - 这意味着：当前 `loopTask`（100 ms）与 `netTask`（100 ms）每秒 10~20 次的错位唤醒，不仅打断了 Light Sleep，而且**每一次唤醒都在反复开关 480 MHz BBPLL**（带来额外的 ~40 µs 切频开销与 PLL 电流尖峰）。因此**减少每秒唤醒次数是收益最高的省电手段**。
2. **`min_freq_mhz` 可进一步下探至 10 MHz 或 20 MHz（XTAL 整数分频）**：
   - 官方文档指出：`min_freq_mhz` 在无锁且未进入 Light Sleep 时生效（例如开机前 45 s 的 `kBootWindowMs` 窗口、`waveshare_rlcd_pmoff` 构建、或空闲时长短于 `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP` 时），可设为 XTAL 的整数分频，最低 **10 MHz**（维持 1 MHz `REF_TICK` 所需的下限）。将 `kIdleMinFreqMhz` 从 `40` 降为 `20` 或 `10`，可直接降低非睡眠空闲态的时钟功耗。
3. **`CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP` 默认阈值为 3 ticks（3 ms）**：
   - 任何小于 3 ms 的休眠（如代码中多处 `delay(1)`、`delay(2)`、`vTaskDelay(pdMS_TO_TICKS(2))`）**均不会触发 Tickless Idle / Automatic Light Sleep**，CPU 只会在 Idle 任务里空转。
4. **外设驱动的 PM 锁开销（I2C vs SPI vs USB CDC）**：
   - Arduino 的 `Wire`（`esp32-hal-i2c.c`）底层使用 ESP-IDF `i2c_master_cmd_begin`，根据 ESP-IDF PM 文档，I2C 驱动在每次传输时会自动申请/释放 `ESP_PM_APB_FREQ_MAX` 锁。当前每秒 1 次的 RTC I2C 读取会导致每分钟 60 次无意义的 I2C PM 锁操作与总线唤醒。

---

## 2. 优化项清单（按收益与优先级排列）

### 优化项一：消除网络任务与主循环的“双 100ms 轮询”错位唤醒（最高优先级）

#### 现状与问题
- **主循环** `AppController::tick()`（`src/app_controller.cpp:421`）末尾固定调用 `delay(100)`。
- **网络任务** `netTask()`（`src/net_worker.cpp:377`）末尾固定调用 `vTaskDelay(pdMS_TO_TICKS(streaming ? 2 : 100))`。
- 两个任务无相位同步，导致系统每秒从 Light Sleep 唤醒 10~20 次并频繁启停 480 MHz BBPLL。
- `netTask` 在非流式播放（`!streaming`）时，每 100 ms 都在调 `WiFi.status()`、`WiFi.RSSI()`、`WiFi.localIP().toString()`（每次循环在堆上构造临时 `String` 并加互斥锁拷贝 `gSnapshot.network`）。

#### 改造思路
1. **网络任务（`src/net_worker.cpp`）改为 `ulTaskNotifyTake` 事件驱动休眠**：
   - 保存 `netTask` 的 `TaskHandle_t gNetTaskHandle = nullptr;`。
   - 当 `streaming == true` 时：若仍有空槽需下载则立即继续，若槽已满等待消费则休眠 `pdMS_TO_TICKS(5)`（$\ge 3$ ticks 以满足 `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP`）。
   - 当 `streaming == false` 时：计算距离下一次 `pollOnce()` 或 `reconnect` 的剩余时间 `waitMs`（例如 `min(remainingToNextPollMs, 1000UL)`，最少不低于 `50 ms`），调用 `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs))` 挂起任务。
   - 在 `netWorkerSetFocus()`（当 `changed == true` 时）、`netWorkerSetFastPoll()`、`netWorkerStartStream()`、`netWorkerStopStream()` 中调用 `if (gNetTaskHandle) xTaskNotifyGive(gNetTaskHandle);`，实现零延迟唤醒。
   - 仅当 `status != lastStatus`（连接状态改变）或到达周期（如每次 `pollOnce()` 前后、或每 2~5 秒）才更新 `network.ip = WiFi.localIP().toString().c_str()`，避免每轮循环堆分配。
2. **主循环（`src/app_controller.cpp`）动态休眠步进**：
   - `pmSetupButtonWakeup()` 已为 `BOOT`（GPIO0）和 `KEY`（GPIO18）配置了 `GPIO_INTR_LOW_LEVEL` 唤醒 Light Sleep（按下按钮会立即打断芯片的 Light Sleep）。
   - 当有按键正在按下或手势进行中（`gBootButton.pressed || gKeyButton.pressed || gEggChord.active || gEggRequestedAt != 0`）时，保持 `delay(50)` 以确保长按（1500 ms / 5000 ms）和双键组合（2000 ms）计时精度。
   - 当无按键按下且非彩蛋播放时，可将空闲 `delay` 放宽至 `200~250 ms`（甚至在已 armed Light Sleep 时利用按键唤醒立即响应首次按下）。

---

### 优化项二：传感器采样降频、温湿度显示死区与无变化重绘消除（高优先级）

#### 现状与问题
1. **每秒 1 次的外部 RTC I2C 读取（`src/app_controller.cpp:184-194`）**：
   - `kClockSampleMs = 1000` 每秒唤醒 I2C 总线读取 PCF85063（每次读取都会触发 ESP-IDF I2C 驱动申请 `ESP_PM_APB_FREQ_MAX` 锁），但所有页面均已取消秒显，只需分钟级更新。
2. **温湿度浮点比较缺少死区（`src/app_controller.cpp:196-207`）**：
   - `sampleSensors()` 直接用 `gEnvironment.temperatureC != prevTemp || gEnvironment.humidityPct != prevHum` 触发 `gForceRender = true`。
   - 屏幕上实际只显示 `%.1fC` 和 `%.0f%%`。传感器 $0.01^\circ\text{C}$ 的微小波动会导致每 30 秒触发一次整屏重绘 + 15 KB 缓冲区 FNV-1a 哈希计算。
3. **10 秒兜底重绘空跑（`src/app_controller.cpp:402` & `include/app_config.h:38`）**：
   - `kRenderIntervalMs = 10000` 每 10 秒强制跑一次 `deriveDisplayState()` + `gRenderer.render()` + 15 KB `frameHash()`，而所有真实数据变化都已经显式设置了 `gForceRender = true`。
4. **SHTC3 测量命令与阻塞等待（`src/environment_monitor.cpp:21, 138-142`）**：
   - 当前使用 `0x7866`（Normal Mode）并阻塞 `delay(20)`；且唤醒等待 `delay(2)` 小于 3 ticks 无法进入 Tickless Idle（SHTC3 手册唤醒时间仅需 ~240 µs）。

#### 改造思路
1. **温湿度按显示精度量化后再触发重绘（`src/app_controller.cpp`）**：
   - 比较量化后的显示整数值：
     - 温度十分位：`static_cast<int>(lroundf(gEnvironment.temperatureC * 10.0f))`
     - 湿度整数位：`static_cast<int>(lroundf(gEnvironment.humidityPct))`
   - 仅当 `climateValid` 变化或上述量化值变化时才置 `gForceRender = true`（上报给 Bridge 的 `DeviceTelemetry` 仍保留原始浮点值）。
2. **降低 RTC I2C 读取频率（`src/app_controller.cpp`）**：
   - 当 `gEnvironment.clockValid` 为真时，根据当前读到的 `gEnvironment.second` 动态决定下次采样间隔：例如当 `second < 55` 时，下次 RTC 采样推迟 `min(10000, (58 - second) * 1000)` 毫秒；当临近整分（`second >= 55`）或 `!clockValid` 时保持 `1000 ms` 采样。这样可将 I2C 读取次数减少 **85% 以上**，且整分跳变误差仍在 1 秒以内。
3. **放宽兜底重绘周期（`include/app_config.h`）**：
   - 将 `kRenderIntervalMs` 从 `10000`（10 s）调大到 `60000`（60 s）。

---

### 优化项三：HTTP 轮询连接复用（高优先级）

#### 现状与问题
- `fetchPollState()`（`src/bridge_client.cpp:530-559`）每次轮询都在栈上创建局部 `HTTPClient http`，完整经历 **TCP 三次握手 + HTTP 请求/响应 + TCP 四次挥手**，拉长了 `WIFI_PS_MAX_MODEM` 下射频处于高功耗 TX/RX 状态的窗口。

#### 改造思路
- 在 `src/bridge_client.cpp`（`#ifdef ARDUINO` 分支）中，为 `fetchPollState()` 使用静态持久化的 `WiFiClient` 与 `HTTPClient`，并调用 `http.setReuse(true)` 启用 Keep-Alive 连接复用。
- 注意：保持 `parsePollPayload()` 等纯逻辑函数不变，确保 `pio test -e native` 完全兼容。

---

### 优化项四：Wi-Fi 断线重连指数退避（中优先级）

#### 现状与问题
- `netTask()`（`src/net_worker.cpp:306-313`）在 Wi-Fi 断开时，每 `kWifiReconnectMs`（10 s）无条件执行一次 `WiFi.disconnect(false); beginWifi();` 全信道扫描。若路由器关闭或设备外出，射频会持续高耗电扫描直至触发低电保护。

#### 改造思路
1. 在 `src/poll_policy.h` / `src/poll_policy.cpp` 中新增纯函数 `uint32_t wifiReconnectIntervalMs(uint32_t reconnectAttempts)`：
   - 前 3 次重试保持 `kWifiReconnectMs`（10 s）；
   - 随后逐步退避（如 20 s -> 40 s -> 60 s -> 120 s，上限 300 s）。
   - 在 `test/native/` 中为该函数编写单元测试。
2. 当用户主动按键（触发 `netWorkerSetFocus` 或 `netWorkerSetFastPoll(true)`）时，若当前未连接 Wi-Fi，重置 `reconnectAttempts = 0` 并立即唤醒 `netTask` 重连。

---

### 优化项五：ESP-IDF 电源管理配置与 SDK 编译项深化（中优先级，直接源自官方文档）

#### 现状与问题
1. **PM SDK 构建配置（`tools/pm-libs/README.md`）仅启用了最基础的两个宏**：
   - 目前只添加了 `CONFIG_PM_ENABLE=y` 和 `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`。
   - 根据 ESP-IDF Power Management 文档，每次从自动 Light Sleep 唤醒（包括每个 Wi-Fi DTIM Beacon 唤醒和每次 FreeRTOS 定时器唤醒），如果睡眠进出路径、RTOS Idle 检查路径和 Wi-Fi 睡眠处理代码位于 SPI Flash 中，芯片必须等待 Flash 唤醒并填充 Cache，增加每次唤醒的时间和电流。
2. **缺少 `CONFIG_PM_PROFILING=y` 导致 `esp_pm_dump_locks` 信息不完整**：
   - `src/pm_controller.cpp:224` 已经调用了 `esp_pm_dump_locks(stdout)`，但未开启 `CONFIG_PM_PROFILING=y` 时，它不会输出每个频率档位（`MAX` / `APB_MAX` / `MIN` / `LIGHT_SLEEP`）的精确驻留时间和各 PM 锁的持有时长。
3. **`kIdleMinFreqMhz` 仍为 40 MHz（`src/pm_controller.cpp:69`）**：
   - 在未进入 Light Sleep 的空闲期（如前 45 s 启动窗口），CPU 维持在 40 MHz，而 ESP32-S3 支持下探至 20 MHz 或 10 MHz。
4. **Light Sleep 激活后的 `Serial.printf` 开销（`HWCDC.cpp`）**：
   - 在 `ARDUINO_USB_CDC_ON_BOOT=1` 下，45 s 后进入 Light Sleep 且未插 USB 时，周期性的 `Serial.printf`（如每 3~10 s 的 `[BRIDGE] ok`、每 15 s 的 `[WIFI] ok`）仍会做格式化、加锁写 RingBuffer 并触发 `usb_serial_jtag_ll_txfifo_flush()`；若此前连接过串口且未正常断开，甚至可能在 `HWCDC::write` 中触发 `tx_timeout_ms`（100 ms）阻塞。
5. **彩蛋播放帧间等待 `delay(2)` 小于 Tickless 阈值（`src/app_controller.cpp:392`）**：
   - `delay(2)` 小于默认的 `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP`（3 ticks），且彩蛋模式将 CPU 锁在 240 MHz，帧间（40~50 ms 一帧）完全忙等。

#### 改造思路
1. **代码层（无需重编 SDK 即可生效）**：
   - 在 `src/pm_controller.cpp` 中，将 `kIdleMinFreqMhz` 从 `40` 下调为 `20`（或 `10`），并修正注释说明 `ESP_PM_CPU_FREQ_MAX` 与 `min_freq_mhz` 的真实关系。
   - 在 `src/app_controller.cpp:392`，彩蛋模式下将 `delay(2)` 改为 `delay(5)`（或根据下一帧剩余时间动态 `delay` 5~15 ms），减少 240 MHz 下的空转轮询次数。
   - 在 `net_worker.cpp` / `pm_controller.cpp` 中，当 `pmLightSleepArmed()` 为真且 `!Serial.isPlugged()` 时，抑制高频例行成功日志（如 `[BRIDGE] ok`、`[WIFI] ok`），保留状态变化和错误日志。
2. **SDK 重建文档与配置层（更新 `tools/pm-libs/README.md`）**：
   - 在 `tools/pm-libs/README.md` 的 `defconfig.esp32s3` 建议配置中补充 ESP-IDF 官方推荐的三个 IRAM 睡眠优化项及 Profiling 选项：
     ```ini
     CONFIG_PM_ENABLE=y
     CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
     CONFIG_PM_SLP_IRAM_OPT=y
     CONFIG_PM_RTOS_IDLE_OPT=y
     CONFIG_ESP_WIFI_SLP_IRAM_OPT=y
     CONFIG_PM_PROFILING=y
     ```
   - **硬件避坑说明（写入 `tools/pm-libs/README.md`）**：明确注明 **不要开启 `CONFIG_PM_SLP_DISABLE_GPIO=y`**（除非对 LCD 引脚显式调用 `gpio_sleep_sel_dis()`），否则每次进入自动 Light Sleep 时非唤醒 GPIO 会被断开变成高阻态，导致 ST7305 的 `CS`（GPIO40）和 `RST`（GPIO41）浮空，屏幕画面丢失或闪烁。

---

### 优化项六：低电量保护与 Deep Sleep 路径细节优化（中低优先级）

#### 现状与问题
1. **保护唤醒路径多余的 `delay(250)`（`src/main.cpp:14-20`）**：
   - `setup()` 开头先执行 `Serial.begin(115200); delay(250);`，然后才调用 `batteryProtectionEarlyCheck()`。
   - 低电量保护期间每 600 s 唤醒一次重测 ADC 时，芯片默认运行在 240 MHz，这 250 ms 的无条件等待使单次唤醒耗电增加一倍以上。
2. **进入低电保护画框期间未停用 Wi-Fi（`src/app_controller.cpp:168-181`）**：
   - `enterBatteryProtection()` 在绘制低电量提示并 `delay(200)` 期间，Core 0 上的 `netTask` 仍在运行，可能并发触发射频收发。

#### 改造思路
1. **缩短保护唤醒活跃时间（`src/main.cpp`）**：
   - `setup()` 中先判断 `if (!batteryProtectionWasActive()) { delay(250); }`，再调用 `batteryProtectionEarlyCheck()`；若在保护唤醒中确认恢复并从 `batteryProtectionEarlyCheck()` 返回正常启动，再补一次 `delay(250)`。
2. **进入低电保护前关闭 Wi-Fi（`src/app_controller.cpp` & `src/net_worker.cpp`）**：
   - 新增 `netWorkerStop()`（置停止标志并调用 `WiFi.disconnect(true); WiFi.mode(WIFI_OFF);`），在 `enterBatteryProtection()` 渲染低电量画面前调用，防止射频瞬态电流拉崩电压。

---

## 3. 建议实施顺序（Step-by-Step）

1. **Step 1（低风险速赢）**：实施 **优化项六**（`main.cpp` 保护唤醒跳过 250 ms + 进入保护前关 Wi-Fi）与 **优化项二**（温湿度显示量化死区、RTC 按秒动态降频读取、`kRenderIntervalMs` 放宽到 60 s）。
2. **Step 2（策略与单测）**：实施 **优化项四**（`poll_policy` 新增 `wifiReconnectIntervalMs` 指数退避 + `native` 单元测试）。
3. **Step 3（核心唤醒优化）**：实施 **优化项一**（`netTask` 改为 `ulTaskNotifyTake` 事件驱动休眠 + 消除每次循环 `localIP().toString()` 临时分配 + 主循环空闲步进优化）。
4. **Step 4（网络与 PM 深化）**：实施 **优化项三**（`HTTPClient` `setReuse(true)` 连接复用）与 **优化项五**（`pm_controller.cpp` 调整 `kIdleMinFreqMhz`、未插 USB 且已进入 Light Sleep 时静默高频例行日志、更新 `tools/pm-libs/README.md`）。
5. **Step 5（全面验证）**：执行 `pio test -e native` 及固件环境编译核验，确保零回归。

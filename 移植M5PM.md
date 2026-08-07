# M5PM1 与 M5IOE1 移植说明

## 1. 移植目标

本文记录把 `M5StopWatch-UserDemo-main` 中 M5PM1 电源管理和 M5IOE1 IO 扩展相关代码移植到 `esp32s3-idf5_co5300-qspi_esp-lvgl-adapter_lvgl8` 的结果。

目标工程使用：

- ESP-IDF 6.0.2
- CO5300 QSPI AMOLED
- CST820 电容触摸
- LVGL 8 与 `esp_lvgl_adapter`
- M5PM1 官方驱动 1.0.6
- M5IOE1 官方驱动 1.0.8

移植不包含 UserDemo 的 Mooncake、M5GFX、完整手表 UI、音频编解码器应用层。目标工程继续显示原来的 `lv_demo_widgets()`；M5PM1 和 M5IOE1 的状态通过串口日志输出。

## 2. 关键改动

### 2.1 统一使用新式 I2C 驱动

M5PM1、M5IOE1 和 CST820 共用一条 `i2c_master_bus_handle_t`：

| 信号 | ESP32-S3 引脚 | 说明 |
| --- | ---: | --- |
| I2C SDA | GPIO47 | PMIC、IOE1、CST820 共用 |
| I2C SCL | GPIO48 | PMIC、IOE1、CST820 共用 |
| I2C 端口 | I2C0 | 只创建一次 |

共享总线在 `board_power_init()` 内通过 `i2c_new_master_bus()` 创建，CST820 从 `board_power_get_i2c_bus()` 获取同一个句柄。工程不再为触摸单独创建第二条 I2C 总线，也没有引入旧版 `driver/i2c.h` 驱动。

M5PM1 和 M5IOE1 官方组件原本在清单中强制依赖 `espressif/i2c_bus`。本工程实际使用它们原生提供的 `i2c_master_bus_handle_t` 接口，因此移除了未使用的 `i2c_bus` 构建依赖，避免 ESP-IDF 6 中新旧驱动或重复总线冲突。

### 2.2 修复复位引脚冲突

原目标工程存在以下问题：

- GPIO40 同时配置为 CO5300 QSPI SCK 和 CST820 RESET。
- GPIO15 被配置为 CO5300 RESET，但 M5StopWatch 实际由 M5IOE1 IO5 控制屏幕复位。

移植后：

- CO5300 的 `reset_gpio_num` 设置为 `GPIO_NUM_NC`，由 M5IOE1 IO5 产生复位脉冲。
- CST820 的 `rst_gpio_num` 设置为 `GPIO_NUM_NC`，由 M5IOE1 IO4 产生复位脉冲。
- GPIO40 只承担 QSPI SCK，不再被触摸驱动改变电平。
- GPIO15 不再用于屏幕复位。

### 2.3 初始化顺序

`app_main()` 使用以下顺序：

1. 创建 GPIO47/GPIO48 上的共享 I2C0 总线。
2. 初始化 M5PM1。
3. 初始化 M5IOE1，并写入各输出的安全默认状态。
4. 通过 M5IOE1 IO5 复位 CO5300。
5. 初始化 CO5300 QSPI 总线和面板。
6. 通过 M5IOE1 IO4 复位 CST820。
7. 在共享 I2C 总线上初始化 CST820。
8. 初始化 LVGL 并显示 `lv_demo_widgets()`。
9. 后台任务每秒采样电源信息，每 10 秒输出一次电源状态日志。

M5IOE1 初始化失败后程序不会继续初始化屏幕和触摸，因为此时无法保证供电与复位电平安全。M5PM1 初始化失败会记录错误，但仍允许 M5IOE1 和显示部分继续初始化，便于定位单芯片故障。

## 3. CO5300 与 CST820 引脚

保留目标工程已经修改好的屏幕 QSPI 引脚：

| 功能 | 引脚 |
| --- | ---: |
| CO5300 CS | GPIO39 |
| CO5300 SCK | GPIO40 |
| CO5300 D0 | GPIO41 |
| CO5300 D1 | GPIO42 |
| CO5300 D2 | GPIO46 |
| CO5300 D3 | GPIO45 |
| CO5300 TE | GPIO38 |
| CST820 INT | GPIO13 |
| CO5300 RESET | M5IOE1 IO5 |
| CST820 RESET | M5IOE1 IO4 |

## 4. M5PM1 移植内容

M5PM1 默认 I2C 地址为 `0x6E`。初始化配置与 UserDemo 保持一致：

- 连续两次设置 I2C 空闲休眠时间为 0。
- 电源键单击判断时间设置为 1000 ms。
- 关闭 M5PM1 内部看门狗。
- 启用 LDO 掉电保持，为 RTC 等电路保留电源。
- 启用充电功能。
- PMIC GPIO3 配置为推挽输出低电平，启用硬件 CHG_PROG 配置。
- PMIC GPIO2 配置为无上下拉输入，用于读取 CHG_STAT。
- 禁止电源键单击触发芯片复位。

### 4.1 电池电量

后台任务每秒读取一次 VBAT。为了避免负载瞬态造成日志中的电量频繁跳变，使用 UserDemo 相同的 7:1 滤波：

```text
filtered = (old * 7 + new) / 8
```

电量百分比采用 3300 mV 为 0%、4200 mV 为 100% 的线性估算。这不是库仑计结果，只适合作为近似电量。

### 4.2 充电判断

- `VIN > 4000 mV`：认为外部电源已接入。
- `CHG_STAT == 0`：认为充电芯片正在执行充电。
- 宽松模式只判断外部电源；严格模式同时要求外部电源存在且 CHG_STAT 有效。

## 5. M5IOE1 移植内容

代码首先探测 UserDemo 使用的地址 `0x4F`，失败后再探测数据手册默认地址 `0x6F`。成功后关闭 I2C 空闲休眠，并配置以下功能：

| M5IOE1 引脚 | 板级功能 | 开机默认状态 |
| --- | --- | --- |
| IO1 | CH442E MUX 控制 | 低 |
| IO3 | 音频电源使能 | 高 |
| IO4 | CST820 复位 | 高 |
| IO5 | CO5300/OLED 复位 | 高 |
| IO8 | L3B 显示电源使能 | 高 |
| IO9 | 振动电机 PWM1 | 0%，关闭 |
| IO10 | 扬声器 PA 使能 | 低，关闭 |

扬声器还使用 ESP32 GPIO14 作为第二级使能。`board_power_set_speaker_enabled()` 会按相同状态同时控制 IO10 和 GPIO14，开机不会主动播放声音。

电机 PWM 频率为 5 kHz。非零强度被映射到 25%-100% 占空比，避免占空比太低时电机无法起振。开机不会主动振动。

## 6. 板级 API

所有接口声明在 `main/board_power.h`：

```c
esp_err_t board_power_init(void);
i2c_master_bus_handle_t board_power_get_i2c_bus(void);

bool board_power_pmic_ready(void);
bool board_power_ioe_ready(void);

esp_err_t board_power_reset_display(void);
esp_err_t board_power_reset_touch(void);
esp_err_t board_power_set_display_power(bool enabled);
esp_err_t board_power_set_audio_enabled(bool enabled);
esp_err_t board_power_set_mux(bool level);

esp_err_t board_power_set_motor(uint8_t strength);
esp_err_t board_power_vibrate(uint16_t duration_ms, uint8_t strength);
esp_err_t board_power_stop_vibration(void);
esp_err_t board_power_set_speaker_enabled(bool enabled);

esp_err_t board_power_get_battery_mv(uint16_t *battery_mv);
esp_err_t board_power_get_battery_percent(uint8_t *percent);
esp_err_t board_power_get_vin_mv(uint16_t *vin_mv);
esp_err_t board_power_is_charging(bool strict, bool *charging);
esp_err_t board_power_get_button_state(bool *pressed);
```

设备未初始化时，依赖该设备的控制或查询接口返回 `ESP_ERR_INVALID_STATE`。空输出指针返回 `ESP_ERR_INVALID_ARG`。

## 7. 串口日志

正常启动时可看到类似日志：

```text
I BOARD_POWER: 共享 I2C 初始化完成: SDA=47, SCL=48, port=0
I BOARD_POWER: M5PM1 初始化完成，I2C 地址=0x6E
I BOARD_POWER: M5IOE1 初始化完成，I2C 地址=0x4F，L3B_EN 重试=0
I BOARD_POWER: 已通过 M5IOE1 IO5 复位 CO5300
I M5STOPWATCH: CO5300 QSPI 显示初始化完成 (466x466)，TE=38
I BOARD_POWER: 已通过 M5IOE1 IO4 复位 CST820
I M5STOPWATCH: CST820 触摸初始化完成，中断 GPIO=13
I M5STOPWATCH: M5StopWatch 初始化完成，系统正常运行
I BOARD_POWER: 电源状态: 电池=3980mV(75%), VIN=5000mV, 外部电源=是, 正在充电=是
```

如果 IOE1 不是 `0x4F`，成功日志可能显示 `0x6F`。如果两个地址均无响应，程序会明确记录已探测的两个地址并停止显示初始化。

## 8. ESP-IDF 6.0.2 使用说明

目标工程原有 `dependencies.lock` 是旧 SDK 生成的锁文件，其中仍可能记录 IDF 5.5.3。不要手工修改锁文件。在 ESP-IDF 6.0.2 终端中重新配置，让组件管理器按当前 `idf_component.yml` 重算依赖：

```powershell
idf.py fullclean
idf.py reconfigure
idf.py build
idf.py -p COM端口 flash monitor
```

本次按要求没有执行构建，因此首次使用 IDF 6.0.2 时必须完成上述依赖重算和编译。

### 8.1 Windows 构建目录必须使用短路径

当前源码目录的绝对路径较长。ESP-IDF 6 的 Mbed TLS/TF-PSA-Crypto 会生成多层中间目录，使用工程内默认 `build` 目录时，部分 `.obj.d` 文件的完整路径会超过 Windows 工具链仍可能受限的 260 字符边界，典型错误如下：

```text
fatal error: opening dependency file ...psa_crypto_driver_esp_rsa_ds_utilities.c.obj.d: No such file or directory
```

工程的 `.vscode/settings.json` 已增加：

```json
"idf.buildPathWin": "D:\\idf-build\\m5stopwatch"
```

使用 VS Code 的 ESP-IDF Build 命令时，扩展会把中间文件写入该短目录。使用终端构建时应显式指定同一目录：

```powershell
idf.py -B D:\idf-build\m5stopwatch reconfigure
idf.py -B D:\idf-build\m5stopwatch build
```

旧的工程内 `build` 目录不再参与新构建，可以保留，也可以在确认不需要其中产物后手动删除。仅启用 Windows 注册表的长路径支持不一定能解决所有交叉编译工具的路径限制，短构建目录更稳定。

### 8.2 IDF 6.0.2 驱动组件拆分

IDF 6 将新版 I2C 和 GPIO 驱动分别放入 `esp_driver_i2c` 与 `esp_driver_gpio` 组件。M5PM1、M5IOE1 的 `CMakeLists.txt` 已显式声明这两个依赖，否则会出现以下连锁错误：

```text
fatal error: driver/gpio.h: No such file or directory
error: 'i2c_master_dev_handle_t' was not declared in this scope
```

缺少 `esp_driver_i2c` 时，M5PM1 无法检测到 `driver/i2c_master.h`，会错误进入 legacy I2C 分支；随后出现的 `int` 到 `gpio_num_t` 和 `esp_err_t` 到 `m5pm1_err_t` 转换错误均是同一根因的连锁结果。依赖补齐后使用 IDF 6 的新 `i2c_master` 分支，无需修改这些 legacy 代码。

### 8.3 esp_lvgl_adapter 0.4.3 兼容补丁

当前依赖中的 `esp_lvgl_adapter 0.4.3` 在 IDF 6 分支调用了 `esp_timer_stop_blocking()`，但 IDF 6.0.2 的公开 `esp_timer.h` 并未声明该函数。已在本地组件中改用公开 API `esp_timer_stop()`，并保留原有的 `ESP_ERR_INVALID_STATE` 容错处理。

补丁文件：`managed_components/espressif__esp_lvgl_adapter/src/adapter/esp_lv_adapter.c`。如果以后删除 `managed_components` 并重新下载依赖，需要确认新版组件是否已修复；若仍为 0.4.3，则应重新应用这一处兼容修改。

### 8.4 IDF 6.0.2 LCD 配置字段

IDF 6.0.2 的 `esp_lcd_panel_dev_config_t` 使用 `rgb_ele_order` 指定 RGB/BGR 元素顺序。主程序已按 CO5300 2.0.2 示例改为：

```c
.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
```

旧字段名 `rgb_endian` 在当前结构体中不存在，并会使后续初始化字段发生错位，从而同时产生 `no member named 'rgb_endian'` 和 `initialized field overwritten` 报错。

## 9. 实机验收清单

1. 启动日志能识别 `M5PM1 0x6E` 和 `M5IOE1 0x4F/0x6F`。
2. 屏幕正常点亮，且初始化期间 GPIO40 不被当作复位脚拉低。
3. CST820 能正常上报触摸坐标。
4. 连接和断开 USB 时，VIN 与外部电源日志发生正确变化。
5. 充电时 CHG_STAT 日志显示“正在充电=是”；充满后可能变为“否”，但外部电源仍为“是”。
6. 需要测试电机时短暂调用 `board_power_vibrate(200, 50)`，确认 200 ms 后自动停止。
7. 需要测试扬声器链路时先调用 `board_power_set_speaker_enabled(true)`，测试完成后必须调用 `board_power_set_speaker_enabled(false)`。

## 10. 相关文件

| 文件 | 作用 |
| --- | --- |
| `main/board_power.h` | M5PM1/M5IOE1 的 C API |
| `main/board_power.cpp` | 电源、IOE1、日志与执行器实现 |
| `main/main.c` | 正确的板级初始化顺序及 LVGL 启动入口 |
| `main/CMakeLists.txt` | 主组件源码和官方驱动依赖 |
| `components/M5PM1` | M5Stack M5PM1 1.0.6 官方驱动 |
| `components/M5IOE1` | M5Stack M5IOE1 1.0.8 官方驱动 |

芯片数据手册：

- [M5PM1 电源管理芯片](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/M5PM1_Datasheet_CN.pdf)
- [M5IOE1 IO 扩展管理芯片](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1210/IO_Expander_Datasheet_CN.pdf)

# ChronoCube 智序魔方

> **通过翻转方块切换模式的物理交互番茄钟 | ESP32-C6 + AMOLED 触控**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: ESP32-C6](https://img.shields.io/badge/Platform-ESP32--C6-green.svg)](https://www.espressif.com/en/products/socs/esp32-c6)

---

## 项目简介

**ChronoCube** 是一款「物理交互优先」的个人专注计时终端。通过翻转魔方的不同面来切换工作模式，用身体动作驱动状态流转，减少屏幕依赖，降低专注启动门槛。

- 🧊 **翻转即切换** — 直立/左立/右立/平放/反扣，每种姿态对应一种模式
- 🎯 **番茄工作法** — 内置深度专注（90min）/ 轻量事务（25min）/ 学习成长（45min）
- 🔇 **极简无声** — 物理交互触发，无需频繁看屏幕，沉浸式专注体验
- 📊 **本地优先** — 所有数据存于 SD 卡，不上传云端，隐私闭环
- 🎨 **Catppuccin Mocha 配色** — 低刺激暗色主题，减少视觉干扰

### 硬件载体

**微雪 ESP32-C6 2.16 寸 AMOLED 触控开发板**（原厂成品，无需焊接/改装）

| 组件 | 型号 | 
|------|------|
| MCU | ESP32-C6 RISC-V 32-bit |
| 屏幕 | CO5300 2.16" AMOLED 480×480 QSPI |
| IMU | QMI8658C 6 轴惯性测量 |
| PMU | AXP2101 电源管理 |
| 触控 | CST9220 电容触控 |
| 音频 | ES8311 DAC + I2S 功放 |
| RTC | PCF85063A |

---

## 翻转操作指南

| 姿态 | 模式 | 时长 |
|------|------|------|
| 🧍 直立（底面朝下） | 深度专注 | 90 min |
| 👈 左立（左侧朝下） | 轻量事务 | 25 min |
| 👉 右立（右侧朝下） | 学习成长 | 45 min |
| 👐 平放（屏幕朝上） | 临时暂停 | — |
| 🙃 反扣（屏幕朝下）≥2.5s | 待机 / 结算 | — |

**按键操作**：
- **BOOT 键长按 2s**：锁定 / 解锁设备
- **USER 键单击**：查看今日总专注时长

---

## 快速开始

### 前置条件

- [Arduino IDE 2.x](https://www.arduino.cc/en/software)
- ESP32 Arduino Core 3.x（在 Arduino IDE Boards Manager 中安装 `esp32` by Espressif Systems）
- 以下 Arduino 库（在 Library Manager 中安装）：
  - `lvgl` v9.5.x
  - `PubSubClient`（MQTT 通信，可选）

### 编译 & 烧录

1. 打开 `ChronoCube/ChronoCube_AI.ino`
2. 开发板选择：**ESP32C6 Dev Module**
3. 修改 `config.h` 中的 WiFi 信息（或留空，不使用网络功能）
4. 编译并上传

### SD 卡准备

部分功能需要 SD 卡（建议 FAT32 格式）：

```
/sdcard/ChronoCube/
├── fonts/
│   ├── cn_24.bin        # 中文字库（文泉驿点阵正黑 24×24）
│   └── en_12x24.bin     # 英文字库（Cascadia Mono 12×24）
├── chronocube.conf      # 运行时配置文件（可选，参考 chronocube.conf.example）
└── events/              # 事件日志（自动生成）
```

> 字库文件较大，请从 [Releases](https://github.com/YOUR_USERNAME/chronocube/releases) 页面下载。

---

## 项目结构

```
ChronoCube/
├── ChronoCube_AI.ino       # 主入口
├── config.h                # 硬件引脚、参数配置
├── chronocube.conf.example # SD 卡运行时配置模板
├── pose.cpp / .h           # 姿态检测（IMU 9 面识别）
├── timer.cpp / .h          # 番茄钟计时引擎
├── display.cpp / .h        # AMOLED 显示驱动
├── audio.cpp / .h          # I2S 音效播放
├── storage.cpp / .h        # SD 卡日志 & 配置
├── network.cpp / .h        # WiFi + MQTT 通信
├── touch.cpp / .h          # CST9220 触控
├── pmu.cpp / .h            # AXP2101 电源管理
├── keys.cpp / .h           # 物理按键处理
├── font_loader.cpp / .h    # SD 卡字库加载
├── font_adapter.cpp / .h   # 字体适配（中英文混排）
├── font_flash_data.h       # Flash 内置字模
├── debug_console.cpp / .h  # 串口调试控制台
├── screenshot.cpp / .h     # 截屏功能
├── spi_bus_lock.cpp / .h   # SPI 总线仲裁
├── i2c_bsp.cpp / .h        # I2C 总线抽象
├── qmi8658.cpp / .h        # QMI8658 IMU 驱动
├── lvgl_bridge.cpp / .h    # LVGL 桥接层
├── lv_port_disp.cpp / .h   # LVGL 显示端口
├── lv_port_indev.cpp / .h  # LVGL 输入设备端口
├── lv_conf.h               # LVGL 配置
├── ui_lvgl_pro.c / .h      # LVGL UI 界面
├── src/
│   ├── config_loader.cpp/.h  # SD 卡配置加载器
│   └── esp_lcd_sh8601.c/.h  # SH8601/CO5300 AMOLED 驱动
└── ui/
    └── README.md             # UI 设计说明
```

---


## 串口调试控制台

固件内置 23 条调试命令，通过 USB CDC 串口（115200 baud）交互：

```
help          — 命令列表
info          — 系统信息（电量/内存/温度）
pose <0-5>    — 注入姿态
state <s>     — 注入状态
screenshot    — 截屏
key <k>       — 注入按键
```

完整手册：视固件内 `help` 命令输出。

---

## 配置

核心参数在 `config.h` 中定义，也可通过 SD 卡 `chronocube.conf` 在运行时覆盖。配置项包括：

- 番茄周期时长（3 种模式 + 对应休息）
- 姿态检测阈值（角度/加速度/陀螺仪）
- 屏幕策略（息屏延迟/低电弹窗）
- 电源管理（待机/休眠阈值）
- UI 主题色（54 分量 Catppuccin 调色板）

配置文件模板见 `ChronoCube/chronocube.conf.example`。

---

## License

MIT © 2026 Zoe LI

---

## 致谢

- 微雪电子 — 开源硬件与 SDK
- LVGL — 嵌入式 GUI 库
- Catppuccin — 配色主题
- 文泉驿 — 开源中文字体

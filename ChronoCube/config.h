#ifndef CONFIG_H
#define CONFIG_H

#ifdef SIMULATOR_MODE
#include <string.h>
#include <stdio.h>
#else
#include <Arduino.h>
#endif

// ==================== 设备身份 ====================
#define DEVICE_ID_DEFAULT "chronocube-01"  // 设备ID，写入SD卡配置，可由中枢下发修改
#define FIRMWARE_VERSION  "5.3.15"

// ==================== I2C 总线（文档 2.2.1）====================
// 全部挂在 I2C0 一条总线上，按地址区分外设
#define I2C_SDA          8
#define I2C_SCL          7
#define I2C_FREQ_HZ      400000  // 400kHz Fast Mode，IMU/RTC/PMU/触控 都够用

// I2C 地址（7-bit）
#define I2C_ADDR_AXP2101   0x34
#define I2C_ADDR_QMI8658_H 0x6B   // QMI8658 高地址
#define I2C_ADDR_PCF85063  0x51
#define I2C_ADDR_CST9220   0x5A
#define I2C_ADDR_ES8311    0x18   // ES8311 默认 0x18，部分板子 0x19

// ==================== 物理按键（文档 2.2.3）====================
#define PIN_KEY_BOOT   10   // 中键：单击=停止提醒音；长按2s=切换锁定
#define PIN_KEY_USER   9    // 右键：单击=亮屏显示当日总时长；长按2s=切换静音
#define PIN_KEY_PWR    18   // 左键：保留原生电源功能，不做自定义

// ==================== 屏幕 CO5300 QSPI（文档 2.2 + 用户确认）====================
// 实际驱动按 CO5300 处理。微雪官方 Arduino 例程用的是 SH8601 driver code，
// 两款芯片 init 序列和寄存器布局高度兼容，例程里的命令可直接复用。
// 如果发现花屏/不亮再回来改 init 序列。
#define PIN_LCD_CS     15
// LCD_RST 由 AXP2101 ALDO3 电源域控制（开关电源做硬件复位），无独立 GPIO
// LCD_BL 由 AXP2101 ALDO2 控制（音频功放使能），亮度由命令 0x51 调节
// QSPI 4 线模式
#define PIN_LCD_QSPI_SCK  0
#define PIN_LCD_QSPI_D0   1   // MOSI
#define PIN_LCD_QSPI_D1   2   // MISO
#define PIN_LCD_QSPI_D2   3
#define PIN_LCD_QSPI_D3   4
#define LCD_H_RES         480
#define LCD_V_RES         480
#define LCD_BITS_PER_PIXEL 16
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

// ==================== SD 卡 SPI（文档 2.2.1/2.2.2）====================
#define PIN_SD_CS      6
#define PIN_SD_MOSI    1     // 与 LCD QSPI D0 共享
#define PIN_SD_MISO    2     // 与 LCD QSPI D1 共享
#define PIN_SD_SCK     0     // 与 LCD QSPI SCK 共享
// 注意：LCD QSPI 和 SD SPI 共享 SPI2_HOST，靠 CS 区分，必须错开使用

// ==================== CST9220 触控（文档 2.2.2）====================
#define PIN_TP_RST     11
#define PIN_TP_INT     5     // 与 LCD QSPI D3 同号，若 LCD 不开 QSPI 模式 4 线可忽略冲突

// ==================== QMI8658 IMU（文档 2.2.2）====================
#define PIN_IMU_INT1   16   // 中断输出，MVP 轮询不用
#define PIN_IMU_INT2   17

// ==================== 音频 I2S（文档 2.2.2）====================
// 引脚映射参考官方 boards/codec_board/board_cfg.h：
//   i2s: {mclk: 19, bclk: 20, ws: 22, din: 21, dout: 23}
// 修复：DOUT(ESP→喇叭) 从 GPIO21→23，DIN(麦克→ESP) 从 GPIO23→21
#define PIN_I2S_MCLK   19
#define PIN_I2S_SCLK   20   // BCK
#define PIN_I2S_LRCK   22   // WS
#define PIN_I2S_DOUT   23   // 数据给喇叭（ASDOUT，匹配官方 dout: 23）
#define PIN_I2S_DIN    21   // 麦克风数据进（DSDIN，匹配官方 din: 21）
// PA_CTRL 由 AXP2101 ALDO2 控制，EN 一下就好

// ==================== 姿态检测参数（文档 4.1）====================
#define POSE_SAMPLE_HZ         20      // 采样频率
#define POSE_PREDELAY_MS       200     // 一阶预反馈（短音效+微亮）
#define POSE_CONFIRM_MS          1500    // 二阶正式确认（普通姿态）
#define POSE_FACEDOWN_CONFIRM_MS 2500   // 反扣需维持 2.5s 防误触
#define POSE_ANGLE_THRESHOLD     25.0f  // 夹角阈值（度）
#define POSE_MOTION_FILTER_G   0.25f   // 矢量与1g偏差>此值视为运动中，姿态保持
#define POSE_RAW_VALID_MIN     0.5f    // 加速度矢量低于此值视为自由落体
#define POSE_RAW_VALID_MAX     1.5f    // 高于此值视为剧烈运动
#define POSE_GYRO_FLIP_DPS     100.0f  // 陀螺仪角速度矢量和 > 此值视为快速翻转 (°/s)，缩短确认时间
#define POSE_GYRO_STILL_DPS    15.0f   // 陀螺仪角速度矢量和 < 此值视为静止 (°/s)，加速确认
#define POSE_FLIP_FAST_MS      800     // 快速翻转时缩短的确认时间 (ms)

// ==================== 番茄周期（秒，文档 4.2）====================
#define MODE_DEEP_FOCUS_WORK   5400   // 深度专注 90 分钟
#define MODE_DEEP_FOCUS_REST   1200   // 休息 20 分钟
#define MODE_LIGHT_WORK_WORK   1500   // 轻量事务 25 分钟
#define MODE_LIGHT_WORK_REST   300    // 休息 5 分钟
#define MODE_STUDY_WORK        2700   // 学习成长 45 分钟
#define MODE_STUDY_REST        600    // 休息 10 分钟

// ==================== 低效专注阈值（20%）====================
#define INEFFECTIVE_RATIO  0.2f   // 周期完成度 < 此值 → 标记低效

// ==================== 屏幕策略（文档 4.3）====================
#define SCREEN_OFF_DELAY_MS    10000  // 无操作 10s 自动息屏
#define SCREEN_FOCUS_OFF_MS    3000   // 专注/休息亮 3s 后息屏
#define SCREEN_FORCE_OFF_MS    300000 // 连续计时 5min 强制息屏
#define SCREEN_TOTAL_POPUP_MS  3000   // S9 总时长弹窗 3s
#define SCREEN_LOWBAT_POPUP_MS 2000   // 低电量提示 2s
#define POWER_SAVE_ENABLED     0      // 省电模式开关（1=启用熄屏，0=常亮），默认常亮

// ==================== 情绪选择（文档 6.1）====================
#define EMOTION_TIMEOUT_MS     10000  // 10s 无操作默认「平淡」

// ==================== 按键（文档 4.5）====================
#define KEY_LONGPRESS_MS       2000

// ==================== 电源管理（文档 4.8）====================
#define POWER_L1_IDLE_MS       300000  // 5min 无操作 → L1
#define POWER_L2_FLIP_MS       60000   // 反扣 1min → L2
#define POWER_L2_IDLE_MS       1800000 // 30min 无操作 → L2
#define POWER_BATTERY_LOW_PCT  10      // 低电提醒
#define POWER_BATTERY_SHUT_PCT 5       // 安全关机
#define POWER_BATTERY_CRIT_PCT 2       // 强制待机阈值

// ---- 功耗分级枚举（TASK-A4）----
enum PowerLevel {
  POWER_L0_NORMAL = 0,   // 正常模式：全功能，屏幕点亮，WiFi活跃
  POWER_L1_STANDBY,      // 待机：屏幕关，IMU 10Hz，WiFi保持
  POWER_L2_DEEP_SLEEP,   // 深度休眠：WiFi断，外设大部分关，IMU 1Hz
  POWER_L3_SHUTDOWN      // 安全关机：保存数据，全员关闭，仅充电检测
};

// ==================== 通信（文档 4.7）====================
#define MQTT_KEEPALIVE_S       60
#define MQTT_RECONNECT_MS      10000
#define WIFI_RECONNECT_MS      10000
#define MQTT_QOS_UP            1   /* 注: PubSubClient 仅支持 QoS 0，此宏保留供将来迁移用 */
#define MQTT_QOS_DOWN          0
#define MQTT_TOPIC_REPORT_FMT  "chronocube/%s/report"
#define MQTT_TOPIC_CMD_FMT     "chronocube/%s/cmd"
#define MQTT_TOPIC_INFO_FMT    "chronocube/%s/info"
#define MQTT_TOPIC_ACK_FMT     "chronocube/%s/ack"

// ==================== WiFi 配置 ====================
// 请修改为你的 WiFi 信息，或使用 SD 卡配置文件 chronocube.conf 覆盖
#define WIFI_SSID   "YOUR_WIFI_SSID"
#define WIFI_PASS   "YOUR_WIFI_PASSWORD"

// ==================== MQTT Broker 配置 ====================
// MQTT 中枢服务（可选，不需要远程同步可忽略）
#define MQTT_HOST   "your-mqtt-broker.local"
#define MQTT_PORT   1883 // MQTT 端口，默认 1883
#define MQTT_USERNAME ""   // MQTT 用户名（为空则不使用认证）
#define MQTT_PASSWORD ""   // MQTT 密码（为空则不使用认证）

// ==================== LVGL UI 开关 =====================
// 启用 LVGL 9.5 UI（取代原 UIManager 手绘）
// 需在 Arduino Library Manager 安装 lvgl 版本 9.5.x
#define USE_LVGL

// ==================== 存储（文档 4.6）====================
#define STORAGE_KEEP_DAYS      7
#define STORAGE_CRC_POLY       0xA001  // CRC16-Modbus

// ==================== Catppuccin Mocha 配色（RGB565）====================
// 使用 display.cpp 的 RGB 宏：CAT_RGB(r,g,b) 转换为 16-bit RGB565
#define CAT_BASE        0x18C3  // #1e1e2e → R:30 G:30 B:46
#define CAT_MANTLE      0x10A2  // #181825 → R:24 G:24 B:37
#define CAT_CRUST       0x0861  // #11111b → R:17 G:17 B:27
#define CAT_TEXT        0xE73C  // #cdd6f4 → R:205 G:214 B:244
#define CAT_SUBTEXT0    0x94B2  // #a6adc8 → R:166 G:173 B:200
#define CAT_SURFACE0    0x3186  // #313244 → R:49 G:50 B:68
#define CAT_SURFACE1    0x4A69  // #45475a → R:69 G:71 B:90
#define CAT_OVERLAY0    0x6B4D  // #6c7086 → R:108 G:112 B:134
#define CAT_GREEN       0x9ED3  // #a6e3a1 → R:166 G:227 B:161
#define CAT_TEAL        0x9753  // #94e2d5 → R:148 G:226 B:213
#define CAT_BLUE        0x8D3F  // #89b4fa → R:137 G:180 B:250
#define CAT_YELLOW      0xFF16  // #f9e2af → R:249 G:226 B:175
#define CAT_PEACH       0xFC40  // #fab387 → R:250 G:179 B:135
#define CAT_PINK        0xF6B5  // #f5c2e7 → R:245 G:194 B:231
#define CAT_MAUVE       0xD5B6  // #cba6f7 → R:203 G:166 B:247
#define CAT_RED         0xE94C  // #f38ba8 → R:243 G:139 B:168
#define CAT_MAROON      0xEB89  // #eba0ac → R:235 G:160 B:172
#define CAT_LAVENDER    0xC79E  // #b4befe → R:180 G:190 B:254
#define CAT_SAPPHIRE    0x761F  // #74c7ec → R:116 G:199 B:236

// 语义色映射（业务层用）
#define COLOR_BG            CAT_BASE
#define COLOR_BG_TOPBAR     CAT_MANTLE
#define COLOR_TEXT          CAT_TEXT
#define COLOR_SUBTEXT       CAT_SUBTEXT0
#define COLOR_DIVIDER       CAT_OVERLAY0
#define COLOR_PROGRESS_BG   CAT_SURFACE0

// 状态强调色
#define COLOR_DEEP_FOCUS    CAT_GREEN
#define COLOR_LIGHT_WORK    CAT_TEAL
#define COLOR_STUDY         CAT_BLUE
#define COLOR_PAUSE         CAT_PEACH
#define COLOR_REST          CAT_TEAL
#define COLOR_EMOTION       CAT_PINK
#define COLOR_TOTAL         CAT_BLUE
#define COLOR_LOCK          CAT_MAUVE
#define COLOR_LOWBAT        CAT_RED

// 情绪按钮色
#define COLOR_EMO_FLOW      CAT_GREEN    // 顺畅
#define COLOR_EMO_STUCK     CAT_SUBTEXT0  // 卡壳（灰色，Owner 2026-07-24 改为灰色，对齐 UI_implementation_guide_v1.0.md）
#define COLOR_EMO_PLAIN     CAT_SURFACE1 // 平淡
#define COLOR_EMO_DRAINED   CAT_MAROON   // 耗竭

// ==================== 系统状态枚举（v5.0 9 个核心状态）====================
enum SystemState {
  STATE_STANDBY = 0,       // S0 待机
  STATE_DEEP_FOCUS,        // S1 深度专注
  STATE_LIGHT_WORK,        // S2 轻量事务
  STATE_STUDY,             // S3 学习成长
  STATE_PAUSE,             // S4 临时暂停
  STATE_DEEP_REST,         // S5 深度休息
  STATE_LIGHT_REST,        // S6 轻量休息
  STATE_STUDY_REST,        // S7 学习休息
  STATE_EMOTION_PICK       // S8 情绪选择
};

// ==================== 情绪标签枚举 ====================
enum EmotionTag {
  EMOTION_FLOW = 0,        // 顺畅
  EMOTION_STUCK,           // 卡壳
  EMOTION_PLAIN,           // 平淡
  EMOTION_DRAINED          // 耗竭
};

// ==================== 事件类型（文档 4.6/4.7 上行数据）====================
enum EventType {
  EVT_BOOT          = 0,
  EVT_POSE_CHANGE   = 1,
  EVT_CYCLE_START   = 2,
  EVT_CYCLE_TICK    = 3,
  EVT_CYCLE_END     = 4,
  EVT_CYCLE_BREAK   = 5,
  EVT_EMOTION_PICK  = 6,
  EVT_REST_START    = 7,
  EVT_REST_END      = 8,
  EVT_REST_SKIP     = 9,
  EVT_KEY_CLICK     = 10,
  EVT_KEY_LONG      = 11,
  EVT_LOCK          = 12,
  EVT_LOW_BATTERY   = 13,
  EVT_REVIEW_DUE    = 14,
  EVT_DAILY_RESET   = 15
};

// ==================== 音频事件（文档 4.4）====================
enum AudioEvent {
  AUDIO_POSE_CONFIRM,   // 姿态确认音
  AUDIO_FOCUS_START,    // 专注开始音
  AUDIO_CYCLE_END,      // 专注周期结束
  AUDIO_REST_END,       // 休息结束
  AUDIO_REST_START,     // 开始休息
  AUDIO_PAUSE,          // 暂停音
  AUDIO_RESUME,         // 恢复音
  AUDIO_KEY_TICK,       // 按键音
  AUDIO_LOW_BATTERY,    // 低电量警告
  AUDIO_REVIEW_DUE,     // 待复盘提醒
  AUDIO_EMOTION_TIMEOUT, // 情绪选择超时（独立于 POSE_CONFIRM）
  AUDIO_LOCK,           // 锁定音（专用，不再复用 KEY_TICK）
  AUDIO_UNLOCK          // 解锁音（专用，不再复用 RESUME）
};

#endif // CONFIG_H

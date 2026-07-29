#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <Arduino.h>
#include <stdint.h>
#include "config.h"
#ifdef USE_LVGL
#include <lvgl.h>
#endif

// ==================== 运行时可覆盖参数 ====================
// 这些参数由 SD 卡 /sdcard/ChronoCube/chronocube.conf 在启动时加载覆盖默认值
// 如果配置文件不存在或文件中未定义某参数，使用 config.h 中的硬编码默认值

struct RuntimeConfig {
  // ---- 番茄周期（秒）----
  unsigned long deepFocusWork;      // MODE_DEEP_FOCUS_WORK   默认 5400
  unsigned long deepFocusRest;      // MODE_DEEP_FOCUS_REST   默认 1200
  unsigned long lightWorkWork;      // MODE_LIGHT_WORK_WORK   默认 1500
  unsigned long lightWorkRest;      // MODE_LIGHT_WORK_REST   默认 300
  unsigned long studyWork;          // MODE_STUDY_WORK        默认 2700
  unsigned long studyRest;          // MODE_STUDY_REST        默认 600

  // ---- 屏幕策略（毫秒）----
  bool powerSaveEnabled;            // POWER_SAVE_ENABLED     默认 false（常亮，不熄屏）；省电模式需用户主动开启：powersave 命令 / MQTT 下发
  unsigned long screenOffDelay;     // SCREEN_OFF_DELAY_MS    默认 10000
  unsigned long screenFocusOff;     // SCREEN_FOCUS_OFF_MS    默认 3000
  unsigned long screenForceOff;     // SCREEN_FORCE_OFF_MS    默认 300000
  unsigned long screenTotalPopup;   // SCREEN_TOTAL_POPUP_MS  默认 3000
  unsigned long screenLowbatPopup;  // SCREEN_LOWBAT_POPUP_MS 默认 2000

  // ---- 按键（毫秒）----
  unsigned long keyLongpress;       // KEY_LONGPRESS_MS       默认 2000

  // ---- 情绪选择（毫秒）----
  unsigned long emotionTimeout;     // EMOTION_TIMEOUT_MS     默认 10000

  // ---- 低效专注阈值（比例 0.0~1.0）----
  float ineffectiveRatio;          // INEFFECTIVE_RATIO      默认 0.2

  // ---- 姿态检测 ----
  float poseAngleThreshold;         // POSE_ANGLE_THRESHOLD   默认 25.0
  float poseMotionFilterG;          // POSE_MOTION_FILTER_G   默认 0.25
  float poseGyroFlipDps;            // POSE_GYRO_FLIP_DPS     默认 100.0
  float poseGyroStillDps;           // POSE_GYRO_STILL_DPS    默认 15.0
  unsigned long poseConfirmMs;      // POSE_CONFIRM_MS        默认 1500
  unsigned long poseFacedownConfirmMs; // POSE_FACEDOWN_CONFIRM_MS 默认 2500
  unsigned long poseFlipFastMs;     // POSE_FLIP_FAST_MS      默认 800
  unsigned long posePredelayMs;     // POSE_PREDELAY_MS       默认 200

  // ---- 电源管理（毫秒）----
  unsigned long powerL1IdleMs;      // POWER_L1_IDLE_MS       默认 300000
  unsigned long powerL2FlipMs;      // POWER_L2_FLIP_MS       默认 60000
  unsigned long powerL2IdleMs;      // POWER_L2_IDLE_MS       默认 1800000
  uint8_t powerBatteryLowPct;       // POWER_BATTERY_LOW_PCT  默认 10
  uint8_t powerBatteryShutPct;      // POWER_BATTERY_SHUT_PCT 默认 5
  uint8_t powerBatteryCritPct;      // POWER_BATTERY_CRIT_PCT 默认 2

  // ---- 存储 ----
  uint8_t storageKeepDays;          // STORAGE_KEEP_DAYS      默认 7

  // ---- UI 主题色（Catppuccin Mocha 色板，RGB 0-255）----
  // 背景色
  uint8_t cpMantleR, cpMantleG, cpMantleB;       // Mantle 深背景  默认 24,24,37
  uint8_t cpBaseR, cpBaseG, cpBaseB;             // Base 主背景  默认 30,30,46
  uint8_t cpSurface0R, cpSurface0G, cpSurface0B;  // 表面0  默认 49,50,68
  uint8_t cpSurface1R, cpSurface1G, cpSurface1B;  // 表面1  默认 69,71,90
  uint8_t cpSurface2R, cpSurface2G, cpSurface2B;  // 表面2  默认 88,91,112
  // 文字色
  uint8_t cpTextR, cpTextG, cpTextB;             // 主文字  默认 205,214,244
  uint8_t cpSubtext0R, cpSubtext0G, cpSubtext0B;  // 副文字0 默认 186,194,222
  uint8_t cpSubtext1R, cpSubtext1G, cpSubtext1B;  // 副文字1 默认 148,156,183
  // 状态色
  uint8_t cpGreenR, cpGreenG, cpGreenB;           // 专注    默认 166,227,161
  uint8_t cpBlueR, cpBlueG, cpBlueB;             // 预留色  默认 137,180,250（当前未使用，休息实际用 Green/Peach/Mauve）
  uint8_t cpMauveR, cpMauveG, cpMauveB;           // 总结    默认 203,166,247
  uint8_t cpYellowR, cpYellowG, cpYellowB;        // 暂停    默认 249,226,175
  uint8_t cpTealR, cpTealG, cpTealB;             // 状态5   默认 148,226,213
  uint8_t cpSapphireR, cpSapphireG, cpSapphireB;   // 状态6   默认 116,199,236
  uint8_t cpLavenderR, cpLavenderG, cpLavenderB;   // 状态7   默认 180,190,254
  uint8_t cpPeachR, cpPeachG, cpPeachB;           // 情绪    默认 250,179,135
  uint8_t cpRedR, cpRedG, cpRedB;                // 警告    默认 243,139,168
  uint8_t cpMaroonR, cpMaroonG, cpMaroonB;        // 耗竭    默认 235,160,172

  // ---- 屏幕亮度（0-100）----
  uint8_t defaultBrightness;          // 开机默认亮度        默认 50

  // ---- 状态标记 ----
  bool loaded;                      // 是否成功从 SD 卡加载
};

// ==================== 对外接口 ====================

// 初始化：从 SD 卡 /sdcard/ChronoCube/chronocube.conf 加载配置
bool configLoaderBegin();

// 重新加载
bool configReloadFromSD();

// 将当前运行时配置写回 SD 卡
bool configSaveToSD();

// 重置为固件默认值
void configResetToDefault();

// 打印当前运行时配置
void configPrintRuntime();

// 重建 LVGL 主题色
void configApplyColors();

// 运行时切换省电模式（机内设置/调试台热切换）
void configSetPowerSave(bool en);

// 获取运行时配色数组（18 色）
#ifdef USE_LVGL
const lv_color_t* configGetUIColors();
#endif

#ifdef __cplusplus
// C++ only: 获取运行时配置的只读引用
const RuntimeConfig& configGetRuntime();
#endif

#endif // CONFIG_LOADER_H

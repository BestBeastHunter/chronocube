#include "config_loader.h"
#include "spi_bus_lock.h"
#ifdef USE_LVGL
#include <lvgl.h>
#endif
#include <stdio.h>
#include <string.h>

// ==================== 全局单例 ====================
static RuntimeConfig g_runtime;
static bool g_initialized = false;

// ==================== 内部工具函数 ====================

// 极简 key=value 解析器
// 格式: key=value  (支持整数、浮点)
// 注释: 以 # 开头的行为注释行
// 空白行: 跳过
static bool parseLine(const char *line, const char *key, char *valOut, size_t valLen) {
  // 跳过前导空白
  while (*line == ' ' || *line == '\t') line++;

  // 跳过注释和空行
  if (*line == '#' || *line == '\0' || *line == '\n' || *line == '\r') return false;

  // 匹配 key=
  size_t keyLen = strlen(key);
  if (strncmp(line, key, keyLen) != 0) return false;
  if (line[keyLen] != '=') return false;

  const char *val = line + keyLen + 1;
  size_t i = 0;
  while (val[i] && val[i] != '\n' && val[i] != '\r' && val[i] != '#' && i < valLen - 1) {
    valOut[i] = val[i];
    i++;
  }
  valOut[i] = '\0';

  // 去除尾部空白
  while (i > 0 && (valOut[i-1] == ' ' || valOut[i-1] == '\t')) {
    valOut[i-1] = '\0';
    i--;
  }

  return (i > 0);
}

static unsigned long parseULong(const char *s) {
  return strtoul(s, NULL, 10);
}

static float parseFloat(const char *s) {
  return strtof(s, NULL);
}

static uint8_t parseU8(const char *s) {
  unsigned long v = strtoul(s, NULL, 10);
  return (v > 255) ? 255 : (uint8_t)v;
}

// ==================== 设置默认值 ====================
static void resetToDefaults() {
  // 番茄周期
  g_runtime.deepFocusWork    = MODE_DEEP_FOCUS_WORK;
  g_runtime.deepFocusRest    = MODE_DEEP_FOCUS_REST;
  g_runtime.lightWorkWork    = MODE_LIGHT_WORK_WORK;
  g_runtime.lightWorkRest    = MODE_LIGHT_WORK_REST;
  g_runtime.studyWork        = MODE_STUDY_WORK;
  g_runtime.studyRest        = MODE_STUDY_REST;

  // 屏幕策略
  g_runtime.powerSaveEnabled  = POWER_SAVE_ENABLED;
  g_runtime.screenOffDelay    = SCREEN_OFF_DELAY_MS;
  g_runtime.screenFocusOff    = SCREEN_FOCUS_OFF_MS;
  g_runtime.screenForceOff    = SCREEN_FORCE_OFF_MS;
  g_runtime.screenTotalPopup  = SCREEN_TOTAL_POPUP_MS;
  g_runtime.screenLowbatPopup = SCREEN_LOWBAT_POPUP_MS;
  // 按键
  g_runtime.keyLongpress      = KEY_LONGPRESS_MS;

  // 情绪选择
  g_runtime.emotionTimeout    = EMOTION_TIMEOUT_MS;

  // 低效专注阈值
  g_runtime.ineffectiveRatio  = INEFFECTIVE_RATIO;

  // 姿态检测
  g_runtime.poseAngleThreshold     = POSE_ANGLE_THRESHOLD;
  g_runtime.poseMotionFilterG      = POSE_MOTION_FILTER_G;
  g_runtime.poseGyroFlipDps        = POSE_GYRO_FLIP_DPS;
  g_runtime.poseGyroStillDps       = POSE_GYRO_STILL_DPS;
  g_runtime.poseConfirmMs          = POSE_CONFIRM_MS;
  g_runtime.poseFacedownConfirmMs  = POSE_FACEDOWN_CONFIRM_MS;
  g_runtime.poseFlipFastMs         = POSE_FLIP_FAST_MS;
  g_runtime.posePredelayMs         = POSE_PREDELAY_MS;

  // 电源管理
  g_runtime.powerL1IdleMs      = POWER_L1_IDLE_MS;
  g_runtime.powerL2FlipMs      = POWER_L2_FLIP_MS;
  g_runtime.powerL2IdleMs      = POWER_L2_IDLE_MS;
  g_runtime.powerBatteryLowPct = POWER_BATTERY_LOW_PCT;
  g_runtime.powerBatteryShutPct= POWER_BATTERY_SHUT_PCT;
  g_runtime.powerBatteryCritPct= POWER_BATTERY_CRIT_PCT;

  // 存储
  g_runtime.storageKeepDays    = STORAGE_KEEP_DAYS;

  // UI 主题色（Catppuccin Mocha 默认值）
  g_runtime.cpMantleR = 24;   g_runtime.cpMantleG = 24;   g_runtime.cpMantleB = 37;
  g_runtime.cpBaseR   = 30;   g_runtime.cpBaseG   = 30;   g_runtime.cpBaseB   = 46;
  g_runtime.cpSurface0R = 49; g_runtime.cpSurface0G = 50; g_runtime.cpSurface0B = 68;
  g_runtime.cpSurface1R = 69; g_runtime.cpSurface1G = 71; g_runtime.cpSurface1B = 90;
  g_runtime.cpSurface2R = 88; g_runtime.cpSurface2G = 91; g_runtime.cpSurface2B = 112;
  g_runtime.cpTextR    = 205; g_runtime.cpTextG    = 214; g_runtime.cpTextB    = 244;
  g_runtime.cpSubtext0R= 186; g_runtime.cpSubtext0G= 194; g_runtime.cpSubtext0B= 222;
  g_runtime.cpSubtext1R= 148; g_runtime.cpSubtext1G= 156; g_runtime.cpSubtext1B= 183;
  g_runtime.cpGreenR   = 166; g_runtime.cpGreenG   = 227; g_runtime.cpGreenB   = 161;
  g_runtime.cpBlueR    = 137; g_runtime.cpBlueG    = 180; g_runtime.cpBlueB    = 250;
  g_runtime.cpMauveR   = 203; g_runtime.cpMauveG   = 166; g_runtime.cpMauveB   = 247;
  g_runtime.cpYellowR  = 249; g_runtime.cpYellowG  = 226; g_runtime.cpYellowB  = 175;
  g_runtime.cpTealR    = 148; g_runtime.cpTealG    = 226; g_runtime.cpTealB    = 213;
  g_runtime.cpSapphireR= 116; g_runtime.cpSapphireG= 199; g_runtime.cpSapphireB= 236;
  g_runtime.cpLavenderR= 180; g_runtime.cpLavenderG= 190; g_runtime.cpLavenderB= 254;
  g_runtime.cpPeachR   = 250; g_runtime.cpPeachG   = 179; g_runtime.cpPeachB   = 135;
  g_runtime.cpRedR     = 243; g_runtime.cpRedG     = 139; g_runtime.cpRedB     = 168;
  g_runtime.cpMaroonR  = 235; g_runtime.cpMaroonG  = 160; g_runtime.cpMaroonB  = 172;

  // 屏幕亮度
  g_runtime.defaultBrightness = 50;

  g_runtime.loaded = false;
}

// ==================== 从 SD 卡加载 ====================
static uint32_t crc32_buf(const uint8_t *data, size_t len, uint32_t crc = 0xFFFFFFFF) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
  }
  return crc;
}

static uint32_t crc32_update(uint8_t byte, uint32_t crc) {
  crc ^= byte;
  for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
  return crc;
}

bool configLoaderBegin() {
  resetToDefaults();

  // 尝试从 SD 卡加载（SPI2 总线与 LCD QSPI 共享，需要互斥锁）
  if (!spi2_lock()) {
    Serial.println("[CONFIG] SPI2 lock failed, using defaults");
    g_initialized = true;
    return false;
  }
  FILE *f = fopen("/sdcard/ChronoCube/chronocube.conf", "r");
  if (!f) {
    spi2_unlock();
    Serial.println("[CONFIG] /sdcard/ChronoCube/chronocube.conf not found, using defaults");
    g_initialized = true;
    return false;
  }

  char line[128];
  char val[64];
  int count = 0;
  uint32_t calcCrc = 0xFFFFFFFF;
  bool haveCrc = false;
  uint32_t fileCrc = 0;

  while (fgets(line, sizeof(line), f)) {
    // P2: CRC32 校验（写盘时追加 crc32= 行）。此处仅校验，不匹配仍继续加载
    if (parseLine(line, "crc32", val, sizeof(val))) {
      sscanf(val, "%X", &fileCrc);
      haveCrc = true;
      continue;
    }
    calcCrc = crc32_buf((const uint8_t *)line, strlen(line), calcCrc);
    // 尝试解析每种已知 key
    if (parseLine(line, "deepFocusWork", val, sizeof(val)))
      { g_runtime.deepFocusWork = parseULong(val); count++; continue; }
    if (parseLine(line, "deepFocusRest", val, sizeof(val)))
      { g_runtime.deepFocusRest = parseULong(val); count++; continue; }
    if (parseLine(line, "lightWorkWork", val, sizeof(val)))
      { g_runtime.lightWorkWork = parseULong(val); count++; continue; }
    if (parseLine(line, "lightWorkRest", val, sizeof(val)))
      { g_runtime.lightWorkRest = parseULong(val); count++; continue; }
    if (parseLine(line, "studyWork", val, sizeof(val)))
      { g_runtime.studyWork = parseULong(val); count++; continue; }
    if (parseLine(line, "studyRest", val, sizeof(val)))
      { g_runtime.studyRest = parseULong(val); count++; continue; }

    if (parseLine(line, "powerSaveEnabled", val, sizeof(val)))
      { g_runtime.powerSaveEnabled = (parseU8(val) != 0); count++; continue; }
    if (parseLine(line, "screenOffDelay", val, sizeof(val)))
      { g_runtime.screenOffDelay = parseULong(val); count++; continue; }
    if (parseLine(line, "screenFocusOff", val, sizeof(val)))
      { g_runtime.screenFocusOff = parseULong(val); count++; continue; }
    if (parseLine(line, "screenForceOff", val, sizeof(val)))
      { g_runtime.screenForceOff = parseULong(val); count++; continue; }
    if (parseLine(line, "screenTotalPopup", val, sizeof(val)))
      { g_runtime.screenTotalPopup = parseULong(val); count++; continue; }
    if (parseLine(line, "screenLowbatPopup", val, sizeof(val)))
      { g_runtime.screenLowbatPopup = parseULong(val); count++; continue; }
    if (parseLine(line, "keyLongpress", val, sizeof(val)))
      { g_runtime.keyLongpress = parseULong(val); count++; continue; }

    if (parseLine(line, "emotionTimeout", val, sizeof(val)))
      { g_runtime.emotionTimeout = parseULong(val); count++; continue; }

    if (parseLine(line, "ineffectiveRatio", val, sizeof(val)))
      { g_runtime.ineffectiveRatio = parseFloat(val); count++; continue; }

    if (parseLine(line, "poseAngleThreshold", val, sizeof(val)))
      { g_runtime.poseAngleThreshold = parseFloat(val); count++; continue; }
    if (parseLine(line, "poseMotionFilterG", val, sizeof(val)))
      { g_runtime.poseMotionFilterG = parseFloat(val); count++; continue; }
    if (parseLine(line, "poseGyroFlipDps", val, sizeof(val)))
      { g_runtime.poseGyroFlipDps = parseFloat(val); count++; continue; }
    if (parseLine(line, "poseGyroStillDps", val, sizeof(val)))
      { g_runtime.poseGyroStillDps = parseFloat(val); count++; continue; }
    if (parseLine(line, "poseConfirmMs", val, sizeof(val)))
      { g_runtime.poseConfirmMs = parseULong(val); count++; continue; }
    if (parseLine(line, "poseFacedownConfirmMs", val, sizeof(val)))
      { g_runtime.poseFacedownConfirmMs = parseULong(val); count++; continue; }
    if (parseLine(line, "poseFlipFastMs", val, sizeof(val)))
      { g_runtime.poseFlipFastMs = parseULong(val); count++; continue; }
    if (parseLine(line, "posePredelayMs", val, sizeof(val)))
      { g_runtime.posePredelayMs = parseULong(val); count++; continue; }

    if (parseLine(line, "powerL1IdleMs", val, sizeof(val)))
      { g_runtime.powerL1IdleMs = parseULong(val); count++; continue; }
    if (parseLine(line, "powerL2FlipMs", val, sizeof(val)))
      { g_runtime.powerL2FlipMs = parseULong(val); count++; continue; }
    if (parseLine(line, "powerL2IdleMs", val, sizeof(val)))
      { g_runtime.powerL2IdleMs = parseULong(val); count++; continue; }
    if (parseLine(line, "powerBatteryLowPct", val, sizeof(val)))
      { g_runtime.powerBatteryLowPct = parseU8(val); count++; continue; }
    if (parseLine(line, "powerBatteryShutPct", val, sizeof(val)))
      { g_runtime.powerBatteryShutPct = parseU8(val); count++; continue; }
    if (parseLine(line, "powerBatteryCritPct", val, sizeof(val)))
      { g_runtime.powerBatteryCritPct = parseU8(val); count++; continue; }

    if (parseLine(line, "storageKeepDays", val, sizeof(val)))
      { g_runtime.storageKeepDays = parseU8(val); count++; continue; }

    // UI 主题色 RGB 分量
    if (parseLine(line, "cpMantleR", val, sizeof(val)))
      { g_runtime.cpMantleR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpMantleG", val, sizeof(val)))
      { g_runtime.cpMantleG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpMantleB", val, sizeof(val)))
      { g_runtime.cpMantleB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpBaseR", val, sizeof(val)))
      { g_runtime.cpBaseR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpBaseG", val, sizeof(val)))
      { g_runtime.cpBaseG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpBaseB", val, sizeof(val)))
      { g_runtime.cpBaseB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSurface0R", val, sizeof(val)))
      { g_runtime.cpSurface0R = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSurface0G", val, sizeof(val)))
      { g_runtime.cpSurface0G = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSurface0B", val, sizeof(val)))
      { g_runtime.cpSurface0B = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSurface1R", val, sizeof(val)))
      { g_runtime.cpSurface1R = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSurface1G", val, sizeof(val)))
      { g_runtime.cpSurface1G = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSurface1B", val, sizeof(val)))
      { g_runtime.cpSurface1B = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSurface2R", val, sizeof(val)))
      { g_runtime.cpSurface2R = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSurface2G", val, sizeof(val)))
      { g_runtime.cpSurface2G = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSurface2B", val, sizeof(val)))
      { g_runtime.cpSurface2B = parseU8(val); count++; continue; }
    if (parseLine(line, "cpTextR", val, sizeof(val)))
      { g_runtime.cpTextR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpTextG", val, sizeof(val)))
      { g_runtime.cpTextG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpTextB", val, sizeof(val)))
      { g_runtime.cpTextB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSubtext0R", val, sizeof(val)))
      { g_runtime.cpSubtext0R = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSubtext0G", val, sizeof(val)))
      { g_runtime.cpSubtext0G = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSubtext0B", val, sizeof(val)))
      { g_runtime.cpSubtext0B = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSubtext1R", val, sizeof(val)))
      { g_runtime.cpSubtext1R = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSubtext1G", val, sizeof(val)))
      { g_runtime.cpSubtext1G = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSubtext1B", val, sizeof(val)))
      { g_runtime.cpSubtext1B = parseU8(val); count++; continue; }
    if (parseLine(line, "cpGreenR", val, sizeof(val)))
      { g_runtime.cpGreenR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpGreenG", val, sizeof(val)))
      { g_runtime.cpGreenG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpGreenB", val, sizeof(val)))
      { g_runtime.cpGreenB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpBlueR", val, sizeof(val)))
      { g_runtime.cpBlueR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpBlueG", val, sizeof(val)))
      { g_runtime.cpBlueG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpBlueB", val, sizeof(val)))
      { g_runtime.cpBlueB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpMauveR", val, sizeof(val)))
      { g_runtime.cpMauveR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpMauveG", val, sizeof(val)))
      { g_runtime.cpMauveG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpMauveB", val, sizeof(val)))
      { g_runtime.cpMauveB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpYellowR", val, sizeof(val)))
      { g_runtime.cpYellowR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpYellowG", val, sizeof(val)))
      { g_runtime.cpYellowG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpYellowB", val, sizeof(val)))
      { g_runtime.cpYellowB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpTealR", val, sizeof(val)))
      { g_runtime.cpTealR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpTealG", val, sizeof(val)))
      { g_runtime.cpTealG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpTealB", val, sizeof(val)))
      { g_runtime.cpTealB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSapphireR", val, sizeof(val)))
      { g_runtime.cpSapphireR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSapphireG", val, sizeof(val)))
      { g_runtime.cpSapphireG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpSapphireB", val, sizeof(val)))
      { g_runtime.cpSapphireB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpLavenderR", val, sizeof(val)))
      { g_runtime.cpLavenderR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpLavenderG", val, sizeof(val)))
      { g_runtime.cpLavenderG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpLavenderB", val, sizeof(val)))
      { g_runtime.cpLavenderB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpPeachR", val, sizeof(val)))
      { g_runtime.cpPeachR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpPeachG", val, sizeof(val)))
      { g_runtime.cpPeachG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpPeachB", val, sizeof(val)))
      { g_runtime.cpPeachB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpRedR", val, sizeof(val)))
      { g_runtime.cpRedR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpRedG", val, sizeof(val)))
      { g_runtime.cpRedG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpRedB", val, sizeof(val)))
      { g_runtime.cpRedB = parseU8(val); count++; continue; }
    if (parseLine(line, "cpMaroonR", val, sizeof(val)))
      { g_runtime.cpMaroonR = parseU8(val); count++; continue; }
    if (parseLine(line, "cpMaroonG", val, sizeof(val)))
      { g_runtime.cpMaroonG = parseU8(val); count++; continue; }
    if (parseLine(line, "cpMaroonB", val, sizeof(val)))
      { g_runtime.cpMaroonB = parseU8(val); count++; continue; }

    // 屏幕亮度
    if (parseLine(line, "defaultBrightness", val, sizeof(val)))
      { g_runtime.defaultBrightness = parseU8(val); count++; continue; }
  }

  calcCrc ^= 0xFFFFFFFF;
  if (haveCrc && calcCrc != fileCrc) {
    Serial.printf("[CONFIG] WARNING: CRC mismatch (calc=0x%08X file=0x%08X) — config may be corrupt, using as-is\n", calcCrc, fileCrc);
  }

  fclose(f);
  spi2_unlock();

  g_runtime.loaded = (count > 0);
  g_initialized = true;

  // P1: semantic range validation on loaded values
  if (g_runtime.ineffectiveRatio < 0.0f || g_runtime.ineffectiveRatio > 1.0f) {
    g_runtime.ineffectiveRatio = INEFFECTIVE_RATIO;
  }
  if (g_runtime.poseConfirmMs < 500)       g_runtime.poseConfirmMs = POSE_CONFIRM_MS;
  if (g_runtime.poseFacedownConfirmMs < 500) g_runtime.poseFacedownConfirmMs = POSE_FACEDOWN_CONFIRM_MS;
  if (g_runtime.screenForceOff < 1000)     g_runtime.screenForceOff = SCREEN_FORCE_OFF_MS;
  if (g_runtime.defaultBrightness > 100)   g_runtime.defaultBrightness = 100;
  if (g_runtime.storageKeepDays < 1)       g_runtime.storageKeepDays = STORAGE_KEEP_DAYS;
  if (g_runtime.poseAngleThreshold < 5.0f || g_runtime.poseAngleThreshold > 85.0f)
    g_runtime.poseAngleThreshold = POSE_ANGLE_THRESHOLD;
  // Battery thresholds must be monotonic: low > shut > crit
  if (g_runtime.powerBatteryCritPct < 1)   g_runtime.powerBatteryCritPct = POWER_BATTERY_CRIT_PCT;
  if (g_runtime.powerBatteryShutPct <= g_runtime.powerBatteryCritPct)
    g_runtime.powerBatteryShutPct = g_runtime.powerBatteryCritPct + 2;
  if (g_runtime.powerBatteryLowPct <= g_runtime.powerBatteryShutPct)
    g_runtime.powerBatteryLowPct = g_runtime.powerBatteryShutPct + 3;
  // workTime/restTime minimum: 防止周期为0导致状态机死锁
  if (g_runtime.deepFocusWork < 60)  g_runtime.deepFocusWork = MODE_DEEP_FOCUS_WORK;
  if (g_runtime.deepFocusRest < 60)  g_runtime.deepFocusRest = MODE_DEEP_FOCUS_REST;
  if (g_runtime.lightWorkWork < 60)  g_runtime.lightWorkWork = MODE_LIGHT_WORK_WORK;
  if (g_runtime.lightWorkRest < 60)  g_runtime.lightWorkRest = MODE_LIGHT_WORK_REST;
  if (g_runtime.studyWork < 60)      g_runtime.studyWork = MODE_STUDY_WORK;
  if (g_runtime.studyRest < 60)      g_runtime.studyRest = MODE_STUDY_REST;

  Serial.printf("[CONFIG] loaded %d params from /sdcard/ChronoCube/chronocube.conf\n", count);
  configPrintRuntime();
  return g_runtime.loaded;
}

// ==================== 对外接口 ====================

const RuntimeConfig& configGetRuntime() {
  return g_runtime;
}

bool configReloadFromSD() {
  return configLoaderBegin();
}

bool configSaveToSD() {
  const char *tmpPath = "/sdcard/ChronoCube/chronocube.conf.tmp";
  const char *finalPath = "/sdcard/ChronoCube/chronocube.conf";

  // P0: SPI2 总线互斥 — 与 LCD QSPI 共享，防止写入期间并发显示操作破坏数据
  if (!spi2_lock()) {
    Serial.println("[CONFIG] SPI2 lock failed, abort save");
    return false;
  }

  FILE *f = fopen(tmpPath, "w");
  if (!f) {
    spi2_unlock();
    Serial.println("[CONFIG] failed to open temp file for write");
    return false;
  }

  fprintf(f, "# ChronoCube Runtime Configuration\n");
  fprintf(f, "# Generated by config_loader, edit carefully\n");
  fprintf(f, "# Lines starting with # are comments\n\n");

  fprintf(f, "# ---- Tomato Cycle Durations (seconds) ----\n");
  fprintf(f, "deepFocusWork=%lu\n", g_runtime.deepFocusWork);
  fprintf(f, "deepFocusRest=%lu\n", g_runtime.deepFocusRest);
  fprintf(f, "lightWorkWork=%lu\n", g_runtime.lightWorkWork);
  fprintf(f, "lightWorkRest=%lu\n", g_runtime.lightWorkRest);
  fprintf(f, "studyWork=%lu\n", g_runtime.studyWork);
  fprintf(f, "studyRest=%lu\n", g_runtime.studyRest);

  fprintf(f, "\n# ---- Screen Policy (milliseconds) ----\n");
  fprintf(f, "powerSaveEnabled=%u\n", g_runtime.powerSaveEnabled ? 1 : 0);
  fprintf(f, "screenOffDelay=%lu\n", g_runtime.screenOffDelay);
  fprintf(f, "screenFocusOff=%lu\n", g_runtime.screenFocusOff);
  fprintf(f, "screenForceOff=%lu\n", g_runtime.screenForceOff);
  fprintf(f, "screenTotalPopup=%lu\n", g_runtime.screenTotalPopup);
  fprintf(f, "screenLowbatPopup=%lu\n", g_runtime.screenLowbatPopup);
  fprintf(f, "\n# ---- Keys (milliseconds) ----\n");
  fprintf(f, "keyLongpress=%lu\n", g_runtime.keyLongpress);

  fprintf(f, "\n# ---- Emotion Selection (milliseconds) ----\n");
  fprintf(f, "emotionTimeout=%lu\n", g_runtime.emotionTimeout);

  fprintf(f, "\n# ---- Ineffective Focus Threshold (0.0~1.0) ----\n");
  fprintf(f, "ineffectiveRatio=%.2f\n", g_runtime.ineffectiveRatio);

  fprintf(f, "\n# ---- Pose Detection ----\n");
  fprintf(f, "poseAngleThreshold=%.1f\n", g_runtime.poseAngleThreshold);
  fprintf(f, "poseMotionFilterG=%.2f\n", g_runtime.poseMotionFilterG);
  fprintf(f, "poseGyroFlipDps=%.1f\n", g_runtime.poseGyroFlipDps);
  fprintf(f, "poseGyroStillDps=%.1f\n", g_runtime.poseGyroStillDps);
  fprintf(f, "poseConfirmMs=%lu\n", g_runtime.poseConfirmMs);
  fprintf(f, "poseFacedownConfirmMs=%lu\n", g_runtime.poseFacedownConfirmMs);
  fprintf(f, "poseFlipFastMs=%lu\n", g_runtime.poseFlipFastMs);
  fprintf(f, "posePredelayMs=%lu\n", g_runtime.posePredelayMs);

  fprintf(f, "\n# ---- Power Management (milliseconds / percent) ----\n");
  fprintf(f, "powerL1IdleMs=%lu\n", g_runtime.powerL1IdleMs);
  fprintf(f, "powerL2FlipMs=%lu\n", g_runtime.powerL2FlipMs);
  fprintf(f, "powerL2IdleMs=%lu\n", g_runtime.powerL2IdleMs);
  fprintf(f, "powerBatteryLowPct=%u\n", g_runtime.powerBatteryLowPct);
  fprintf(f, "powerBatteryShutPct=%u\n", g_runtime.powerBatteryShutPct);
  fprintf(f, "powerBatteryCritPct=%u\n", g_runtime.powerBatteryCritPct);

  fprintf(f, "\n# ---- Storage ----\n");
  fprintf(f, "storageKeepDays=%u\n", g_runtime.storageKeepDays);

  fprintf(f, "\n# ---- UI Theme Colors (Catppuccin Mocha palette, RGB 0-255) ----\n");
  fprintf(f, "cpMantleR=%u\ncpMantleG=%u\ncpMantleB=%u\n",
          g_runtime.cpMantleR, g_runtime.cpMantleG, g_runtime.cpMantleB);
  fprintf(f, "cpBaseR=%u\ncpBaseG=%u\ncpBaseB=%u\n",
          g_runtime.cpBaseR, g_runtime.cpBaseG, g_runtime.cpBaseB);
  fprintf(f, "cpSurface0R=%u\ncpSurface0G=%u\ncpSurface0B=%u\n",
          g_runtime.cpSurface0R, g_runtime.cpSurface0G, g_runtime.cpSurface0B);
  fprintf(f, "cpSurface1R=%u\ncpSurface1G=%u\ncpSurface1B=%u\n",
          g_runtime.cpSurface1R, g_runtime.cpSurface1G, g_runtime.cpSurface1B);
  fprintf(f, "cpSurface2R=%u\ncpSurface2G=%u\ncpSurface2B=%u\n",
          g_runtime.cpSurface2R, g_runtime.cpSurface2G, g_runtime.cpSurface2B);
  fprintf(f, "cpTextR=%u\ncpTextG=%u\ncpTextB=%u\n",
          g_runtime.cpTextR, g_runtime.cpTextG, g_runtime.cpTextB);
  fprintf(f, "cpSubtext0R=%u\ncpSubtext0G=%u\ncpSubtext0B=%u\n",
          g_runtime.cpSubtext0R, g_runtime.cpSubtext0G, g_runtime.cpSubtext0B);
  fprintf(f, "cpSubtext1R=%u\ncpSubtext1G=%u\ncpSubtext1B=%u\n",
          g_runtime.cpSubtext1R, g_runtime.cpSubtext1G, g_runtime.cpSubtext1B);
  fprintf(f, "cpGreenR=%u\ncpGreenG=%u\ncpGreenB=%u\n",
          g_runtime.cpGreenR, g_runtime.cpGreenG, g_runtime.cpGreenB);
  fprintf(f, "cpBlueR=%u\ncpBlueG=%u\ncpBlueB=%u\n",
          g_runtime.cpBlueR, g_runtime.cpBlueG, g_runtime.cpBlueB);
  fprintf(f, "cpMauveR=%u\ncpMauveG=%u\ncpMauveB=%u\n",
          g_runtime.cpMauveR, g_runtime.cpMauveG, g_runtime.cpMauveB);
  fprintf(f, "cpYellowR=%u\ncpYellowG=%u\ncpYellowB=%u\n",
          g_runtime.cpYellowR, g_runtime.cpYellowG, g_runtime.cpYellowB);
  fprintf(f, "cpTealR=%u\ncpTealG=%u\ncpTealB=%u\n",
          g_runtime.cpTealR, g_runtime.cpTealG, g_runtime.cpTealB);
  fprintf(f, "cpSapphireR=%u\ncpSapphireG=%u\ncpSapphireB=%u\n",
          g_runtime.cpSapphireR, g_runtime.cpSapphireG, g_runtime.cpSapphireB);
  fprintf(f, "cpLavenderR=%u\ncpLavenderG=%u\ncpLavenderB=%u\n",
          g_runtime.cpLavenderR, g_runtime.cpLavenderG, g_runtime.cpLavenderB);
  fprintf(f, "cpPeachR=%u\ncpPeachG=%u\ncpPeachB=%u\n",
          g_runtime.cpPeachR, g_runtime.cpPeachG, g_runtime.cpPeachB);
  fprintf(f, "cpRedR=%u\ncpRedG=%u\ncpRedB=%u\n",
          g_runtime.cpRedR, g_runtime.cpRedG, g_runtime.cpRedB);
  fprintf(f, "cpMaroonR=%u\ncpMaroonG=%u\ncpMaroonB=%u\n",
          g_runtime.cpMaroonR, g_runtime.cpMaroonG, g_runtime.cpMaroonB);

  fprintf(f, "\n# ---- Screen Brightness (0-100) ----\n");
  fprintf(f, "defaultBrightness=%u\n", g_runtime.defaultBrightness);

  fclose(f);

  // P2: 计算并追加 CRC32 校验行（校验范围 = crc 行之前的所有内容），防止断电/SD 位翻转导致配置损坏被静默使用
  {
    FILE *fr = fopen(tmpPath, "rb");
    if (fr) {
      // 流式计算 CRC：逐字节读取，遇到最后一个 "crc32=" 停止
      uint32_t crc = 0xFFFFFFFF;
      char buf[256];
      size_t totalLen = 0;
      size_t crcStopLen = (size_t)-1;
      bool foundTag = false;

      while (!foundTag && !feof(fr)) {
        size_t n = fread(buf, 1, sizeof(buf), fr);
        if (n == 0) break;

        for (size_t i = 0; i < n && !foundTag; i++) {
          if (totalLen + i >= crcStopLen) break;

          if (buf[i] == 'c' && i + 5 < n &&
              buf[i+1] == 'r' && buf[i+2] == 'c' && buf[i+3] == '3' &&
              buf[i+4] == '2' && buf[i+5] == '=') {
            crcStopLen = totalLen + i;
            foundTag = true;
          } else if (totalLen + i < crcStopLen) {
            crc = crc32_update(buf[i], crc);
          }
        }
        totalLen += n;
      }

      fclose(fr);

      if (!foundTag) {
        crcStopLen = totalLen;
      }

      uint32_t finalCrc = crc ^ 0xFFFFFFFF;
      FILE *fa = fopen(tmpPath, "ab");
      if (fa) { fprintf(fa, "crc32=%08X\n", finalCrc); fclose(fa); }
    }
  }

  // P1: atomic rename to prevent power-loss corruption
  if (rename(tmpPath, finalPath) != 0) {
    spi2_unlock();
    Serial.println("[CONFIG] rename to chronocube.conf failed");
    return false;
  }
  spi2_unlock();
  Serial.println("[CONFIG] saved to /sdcard/ChronoCube/chronocube.conf");
  return true;
}

void configResetToDefault() {
  resetToDefaults();
  Serial.println("[CONFIG] reset to firmware defaults");
}

void configSetPowerSave(bool en) {
  g_runtime.powerSaveEnabled = en;
  Serial.printf("[CONFIG] powerSaveEnabled = %s\n", en ? "true" : "false");
}

void configPrintRuntime() {
  Serial.println("[CONFIG] ---- Runtime Configuration ----");
  Serial.printf("  deepFocusWork=%lu  deepFocusRest=%lu\n",
                g_runtime.deepFocusWork, g_runtime.deepFocusRest);
  Serial.printf("  lightWorkWork=%lu  lightWorkRest=%lu\n",
                g_runtime.lightWorkWork, g_runtime.lightWorkRest);
  Serial.printf("  studyWork=%lu      studyRest=%lu\n",
                g_runtime.studyWork, g_runtime.studyRest);
  Serial.printf("  screenOffDelay=%lu  screenFocusOff=%lu  screenForceOff=%lu\n",
                g_runtime.screenOffDelay, g_runtime.screenFocusOff, g_runtime.screenForceOff);
  Serial.printf("  emotionTimeout=%lu  ineffectiveRatio=%.2f\n",
                g_runtime.emotionTimeout, g_runtime.ineffectiveRatio);
  Serial.printf("  poseAngleThr=%.1f  poseConfirmMs=%lu  poseFacedownMs=%lu\n",
                g_runtime.poseAngleThreshold, g_runtime.poseConfirmMs,
                g_runtime.poseFacedownConfirmMs);
  Serial.printf("  powerL1Idle=%lu  powerL2Flip=%lu  powerL2Idle=%lu\n",
                g_runtime.powerL1IdleMs, g_runtime.powerL2FlipMs, g_runtime.powerL2IdleMs);
  Serial.printf("  battery: low=%u  shut=%u  crit=%u\n",
                g_runtime.powerBatteryLowPct, g_runtime.powerBatteryShutPct,
                g_runtime.powerBatteryCritPct);
  Serial.printf("  storageKeepDays=%u  loaded=%s\n",
                g_runtime.storageKeepDays, g_runtime.loaded ? "yes" : "no");
  Serial.println("[CONFIG] ------------------------------");
}

// ==================== 主题色重建（LVGL v9.5）====================

// 全局颜色变量（configApplyColors 写入，configGetUIColors 读取）
// 运行时主题色切换基础设施已完成（18 色 × 2 字节 = 36B）。
// 现已在 ui_init() 创建屏幕前调用 configApplyColors()，且 ui_lvgl_pro.h 的 CP_* 宏
// 改为经 configGetUIColors() 读取 g_ui_colors，SD 卡主题色覆盖已生效。
#ifdef USE_LVGL
lv_color_t g_ui_colors[18];  // 对应 18 个 CP_* 颜色（索引: 0=Mantle ... 17=Maroon）
#endif

void configApplyColors() {
#ifdef USE_LVGL
  const RuntimeConfig &cfg = configGetRuntime();

  // v9: lv_color_make() still works for RGB565 with 8-bit components
  g_ui_colors[0]  = lv_color_make(cfg.cpMantleR,   cfg.cpMantleG,   cfg.cpMantleB);
  g_ui_colors[1]  = lv_color_make(cfg.cpBaseR,     cfg.cpBaseG,     cfg.cpBaseB);
  g_ui_colors[2]  = lv_color_make(cfg.cpSurface0R, cfg.cpSurface0G, cfg.cpSurface0B);
  g_ui_colors[3]  = lv_color_make(cfg.cpSurface1R, cfg.cpSurface1G, cfg.cpSurface1B);
  g_ui_colors[4]  = lv_color_make(cfg.cpSurface2R, cfg.cpSurface2G, cfg.cpSurface2B);
  g_ui_colors[5]  = lv_color_make(cfg.cpTextR,     cfg.cpTextG,     cfg.cpTextB);
  g_ui_colors[6]  = lv_color_make(cfg.cpSubtext0R, cfg.cpSubtext0G, cfg.cpSubtext0B);
  g_ui_colors[7]  = lv_color_make(cfg.cpSubtext1R, cfg.cpSubtext1G, cfg.cpSubtext1B);
  g_ui_colors[8]  = lv_color_make(cfg.cpGreenR,    cfg.cpGreenG,    cfg.cpGreenB);
  g_ui_colors[9]  = lv_color_make(cfg.cpBlueR,     cfg.cpBlueG,     cfg.cpBlueB);
  g_ui_colors[10] = lv_color_make(cfg.cpMauveR,    cfg.cpMauveG,    cfg.cpMauveB);
  g_ui_colors[11] = lv_color_make(cfg.cpYellowR,   cfg.cpYellowG,   cfg.cpYellowB);
  g_ui_colors[12] = lv_color_make(cfg.cpTealR,     cfg.cpTealG,     cfg.cpTealB);
  g_ui_colors[13] = lv_color_make(cfg.cpSapphireR, cfg.cpSapphireG, cfg.cpSapphireB);
  g_ui_colors[14] = lv_color_make(cfg.cpLavenderR, cfg.cpLavenderG, cfg.cpLavenderB);
  g_ui_colors[15] = lv_color_make(cfg.cpPeachR,    cfg.cpPeachG,    cfg.cpPeachB);
  g_ui_colors[16] = lv_color_make(cfg.cpRedR,      cfg.cpRedG,      cfg.cpRedB);
  g_ui_colors[17] = lv_color_make(cfg.cpMaroonR,   cfg.cpMaroonG,   cfg.cpMaroonB);

  Serial.println("[CONFIG] colors applied (LVGL v9.5)");
#endif
}

#ifdef USE_LVGL
const lv_color_t* configGetUIColors() {
  return g_ui_colors;
}
#endif


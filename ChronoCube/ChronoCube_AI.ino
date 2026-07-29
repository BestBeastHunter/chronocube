/*
 * ChronoCube 智序魔方 - v5.3.7 固件
 * 文档：docs/design/UI_implementation_guide_v1.0.md
 * 板子：微雪 ESP32-C6 Touch AMOLED 2.16
 * 框架：Arduino-ESP32
 * UI：LVGL 9.5 (USE_LVGL) | 原生 UIManager (后备)
 *
 * 姿态-状态映射（业务层决定，非姿态层）：
 *   POSE_FLAT_UP   + 专注中   → S4 暂停
 *   POSE_FLAT_UP   + 暂停中   → 恢复 previousWorkState
 *   POSE_UPRIGHT   + 任何状态 → S1 深度专注（新周期）
 *   POSE_LEFT      + 任何状态 → S2 轻量事务（新周期）
 *   POSE_RIGHT     + 任何状态 → S3 学习成长（新周期）
 *   POSE_FLAT_DOWN + 稳定2.5s → 强制结算，进 S0 待机
 *   S8 情绪选择时：屏蔽所有姿态，仅点按或10s超时确认
 */

#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "i2c_bsp.h"
#include "qmi8658.h"
#include "pose.h"
#include "timer.h"
#include "pmu.h"
#include "display.h"
#include "touch.h"
#include "keys.h"
#include "audio.h"
#include "storage.h"
#include "spi_bus_lock.h"
#include "font_loader.h"
#include "network.h"
#include "src/config_loader.h"

/* ==================== 前向声明 ==================== */
static void onEmotionConfirm(uint8_t idx);
static bool s_emotionTimeout = false;  // v5.3: S8 超时标记

/* ==================== UI 双轨切换 ==================== */
#ifdef USE_LVGL
  #include "lvgl.h"
  #include "lv_port_disp.h"
  #include "lv_port_indev.h"
  #include "lvgl_bridge.h"
  #include "font_adapter.h"
  #include "font_flash_data.h"
  #include "ui_lvgl_pro.h"

  static unsigned long lvglLastTickMs = 0;
  static unsigned long lowBatShownMs = 0;         // 低电量弹窗显示时刻
  static unsigned long totalPopupShownMs = 0;     // S9 弹窗显示时刻

  /* LVGL 兼容包装器 */
  inline void uiSetup() {
    /* LVGL core + display + UI 初始化 */
    lv_init();
    Serial.println("[BOOT] LVGL core OK");
    lv_port_disp_init();
    if (lv_display_get_default() == NULL) {
      Serial.println("[BOOT] LVGL display FAILED (likely OOM) — UI disabled");
    } else {
      Serial.println("[BOOT] LVGL display OK");
    }

    /* 触控输入设备 */
    lv_port_indev_init();

    /* 中文字体：Flash 内嵌 / SD 卡 双路径（v5.5 Flash 字库移植）
     *   FONT_IN_FLASH=1 → 从 PROGMEM 读，零 SD 依赖，零 SPI2 冲突，首帧零卡顿
     *   FONT_IN_FLASH=0 → 从 SD 卡 .bin 读（兼容 v5.3 旧代码） */
#if FONT_IN_FLASH
    {
      int ret24 = cn_font_24_init_flash();
      if (ret24 == 0) Serial.printf("[BOOT] LVGL FLASH font cn_24 OK (%u chars, XIP zero-copy)\n", (unsigned)G_CN24_COUNT);
      else            Serial.printf("[WARN] LVGL FLASH font cn_24 fail ret=%d (PROGMEM empty?)\n", ret24);

      int ret48 = cn_font_48_init_flash();
      if (ret48 == 0) Serial.printf("[BOOT] LVGL FLASH font cn_48 OK (%u chars, native 48x48)\n", (unsigned)G_CN48_COUNT);
      else            Serial.printf("[WARN] LVGL FLASH font cn_48 fail ret=%d\n", ret48);

      int ret96 = en_font_96_init_flash();
      if (ret96 == 0) Serial.printf("[BOOT] LVGL FLASH font en_96 OK (%u digits, native 56x96)\n", (unsigned)G_EN_COUNT);
      else            Serial.printf("[WARN] LVGL FLASH font en_96 fail ret=%d\n", ret96);
    }
#else
    if (storage.isReady()) {
      int ret24 = cn_font_init("/sdcard/ChronoCube/fonts/cn_24.bin");
      if (ret24 == 0) {
        Serial.println("[BOOT] LVGL font cn_24 OK");
      } else {
        Serial.printf("[WARN] LVGL font cn_24 load fail, ret=%d (fallback to built-in)\n", ret24);
      }
      int ret48 = cn_font_48_init("/sdcard/ChronoCube/fonts/cn_48.bin");
      if (ret48 == 0) {
        Serial.println("[BOOT] LVGL font cn_48 OK (native 48x48, no upscale)");
      } else {
        Serial.printf("[WARN] LVGL font cn_48 load fail, ret=%d (cn_48.bin missing?)\n", ret48);
      }
      int ret96 = en_font_96_init("/sdcard/ChronoCube/fonts/en_96.bin");
      if (ret96 == 0) {
        Serial.println("[BOOT] LVGL font en_96 OK (native 56x96 digits)");
      } else {
        Serial.printf("[WARN] LVGL font en_96 load fail, ret=%d (en_96.bin missing?)\n", ret96);
      }
    } else {
      Serial.println("[WARN] SD not mounted, LVGL font unavailable");
    }
#endif /* FONT_IN_FLASH */

    ui_init();
    Serial.println("[BOOT] LVGL UI screens created");

    /* 字形预加载
     *   FONT_IN_FLASH=1 → 内部直接 return（XIP 已足够快，0ms）
     *   FONT_IN_FLASH=0 → 把所有 UI 用到的中文字预读进 LRU cache，
     *                     避免首次渲染时 SD SPI + LCD QSPI 共享 SPI2_HOST 的总线冲突 */
    cn_font_prewarm();

    /* 情绪选择回调（让 LVGL 按钮能调用 onEmotionConfirm） */
    ui_set_emotion_callback(onEmotionConfirm);

    /* 开机锁定 */
    lvglBridge_setLocked(true);
  }

  inline void uiLoop(unsigned long now) {
    if (lvglLastTickMs == 0) lvglLastTickMs = now;
    lv_tick_inc(now - lvglLastTickMs);
    lvglLastTickMs = now;
    lv_timer_handler();
  }

  /* 向 LVGL 桥接层推送当前计时数据（remain/pct/state）
   * 仅在计时相关状态（非 STANDBY/EMOTION_PICK）时有效
   * uiInvalidate() 和 loop() 共用，避免逻辑重复 */
  inline void uiPushTimerData() {
    SystemState st = timerManager.getState();
    if (st == STATE_STANDBY || st == STATE_EMOTION_PICK) return;
    unsigned long remain;
    int pct;
    if (st == STATE_PAUSE) {
      remain = timerManager.getPauseDuration();
      pct = 0;
    } else {
      unsigned long total = timerManager.getCycleTotal();
      unsigned long elapsed = timerManager.getCurrentDuration();
      remain = (elapsed < total) ? (total - elapsed) : 0;
      pct = (int)(timerManager.getProgressPct() * 100.0f);
    }
    lvglBridge_updateData(remain, pct, st);
  }

  inline void uiInvalidate() {
    if (keys.isLocked()) return;  /* 锁定时不允许切换屏幕 */
    if (totalPopupShownMs > 0) return;  /* 弹窗显示期间不切屏 */
    if (lowBatShownMs > 0) return;      /* 低电量弹窗期间不切屏 */
    lvglBridge_showScreenForState(timerManager.getState());
    uiPushTimerData();  /* 首帧快照：切换后立刻推送当前计时数据 */
    /* 立即渲染，消除屏幕切换延迟 */
    unsigned long now = millis();
    if (lvglLastTickMs == 0) lvglLastTickMs = now;
    lv_tick_inc(now - lvglLastTickMs);
    lvglLastTickMs = now;
    lv_timer_handler();
  }
  inline void uiSetScreenOn(bool on) { lvglSetScreenEnabled(on); }
  inline bool uiIsScreenOn() { return lvglIsScreenEnabled(); }
  inline void uiShowTotalPopup() {
    unsigned long eff = timerManager.getEffectiveToday();
    unsigned long ineff = timerManager.getIneffectiveToday();
    unsigned long rest = timerManager.getTotalToday() - eff - ineff;
    totalPopupShownMs = millis();  /* 先设置标记，防止切屏过程中 uiInvalidate() 意外切走 */
    lvglBridge_showSummaryPopup(eff, ineff, rest);
  }
  inline void uiHideTotalPopup() { uiInvalidate(); }
  inline void uiSetLocked(bool lk) { lvglBridge_setLocked(lk); }
  inline void uiShowLowBattery(uint8_t pct) { ui_show_low_battery_pct(pct); lowBatShownMs = millis(); }
  inline void uiSetEmotionSec(uint16_t sec) { ui_set_emotion_countdown(sec); }
  inline void uiResetScreenTimer() { /* LVGL 在 uiLoop 中处理屏超 */ }
  inline void uiUpdateStandbyData() {
    /* S0 待机数据每秒更新（在 timer tick 中调用） */
    if (ui_get_current_screen() == UI_SCREEN_STANDBY) {
      lvglBridge_updateStandby(
        timerManager.getModeToday(STATE_DEEP_FOCUS),
        timerManager.getModeToday(STATE_LIGHT_WORK),
        timerManager.getModeToday(STATE_STUDY),
        timerManager.getPauseDuration()
      );
    }
  }
  inline void uiBegin()    { uiSetup(); }
  inline void uiUpdate()   { unsigned long n = millis(); uiLoop(n); }

#else
  // 原生 UIManager 已归档至 archive/firmware_native_ui/（USE_LVGL 未定义时本分支才编译；ui.h 含 #error 守卫阻止误用）
  #include "ui.h"

  inline void uiBegin()    { ui.begin(); }
  inline void uiUpdate()   { ui.update(); }
  inline void uiInvalidate() { ui.invalidate(); }
  inline void uiSetScreenOn(bool on) { ui.setScreenOn(on); }
  inline bool uiIsScreenOn() { return ui.isScreenOn(); }
  inline void uiShowTotalPopup() { ui.showTotalPopup(); }
  inline void uiHideTotalPopup() { ui.hideTotalPopup(); }
  inline void uiSetLocked(bool lk) { ui.setLocked(lk); }
  inline void uiShowLowBattery() { ui.showLowBattery(); }
  inline void uiSetEmotionSec(uint16_t sec) { ui.setEmotionRemainingSec(sec); }
  inline void uiResetScreenTimer() { ui.resetScreenTimer(); }
  inline void uiUpdateStandbyData() { /* 原生 UI 自行处理 */ }
#endif

// 串口调试控制台（事件注入用）
#include "debug_console.h"

// 前向声明（onMqttCommand 在 logEvent 之前定义）
static void logEvent(const char *action, const char *detail);

static void onMqttCommand(const char *topic, const uint8_t *payload, unsigned int length) {
  char cmd[128] = {0};
  unsigned int copyLen = (length < sizeof(cmd) - 1) ? length : sizeof(cmd) - 1;
  memcpy(cmd, payload, copyLen);
  cmd[copyLen] = '\0';
  Serial.printf("[MQTT] cmd: %s\n", cmd);

  // 提取 "cmd":"xxx" 字段
  const char *cmdKey = strstr(cmd, "\"cmd\"");
  if (!cmdKey) return;
  const char *cmdValStart = strchr(cmdKey, ':');
  if (!cmdValStart) return;
  cmdValStart = strchr(cmdValStart, '"');
  if (!cmdValStart) return;
  cmdValStart++;
  const char *cmdValEnd = strchr(cmdValStart, '"');
  if (!cmdValEnd) return;
  char cmdName[32];
  size_t cmdLen = cmdValEnd - cmdValStart;
  if (cmdLen >= sizeof(cmdName)) cmdLen = sizeof(cmdName) - 1;
  memcpy(cmdName, cmdValStart, cmdLen);
  cmdName[cmdLen] = '\0';

  // 辅助：提取字符串字段值
  auto getStrField = [&](const char *key, char *out, size_t outSize) -> bool {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(cmd, search);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
      p++;
      const char *end = strchr(p, '"');
      if (!end) return false;
      size_t len = end - p;
      if (len >= outSize) len = outSize - 1;
      memcpy(out, p, len);
      out[len] = '\0';
      return true;
    }
    return false;
  };

  // 辅助：提取整数字段值
  auto getIntField = [&](const char *key, long &out) -> bool {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(cmd, search);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
      p++;
      const char *end = strchr(p, '"');
      if (!end) return false;
      char tmp[24];
      size_t len = end - p;
      if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
      memcpy(tmp, p, len);
      tmp[len] = '\0';
      out = atol(tmp);
      return true;
    }
    out = atol(p);
    return true;
  };

  // 辅助：提取布尔字段值
  auto getBoolField = [&](const char *key, bool &out) -> bool {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(cmd, search);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) { out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { out = false; return true; }
    if (*p == '1' || *p == '0') { out = (*p == '1'); return true; }
    return false;
  };

  // ============ 指令分发 ============
  if (strcmp(cmdName, "set_mode") == 0) {
    char mode[32] = {0};
    if (getStrField("mode", mode, sizeof(mode))) {
      if (strcmp(mode, "deep_focus") == 0) {
        // P1-5: MQTT set_mode 必须经过 S8 情绪分类（与物理姿态操作路径一致）
        timerManager.enterFocus(STATE_DEEP_FOCUS, false);  // false=不跳过 S8
        uiInvalidate();
        logEvent("MQTT_MODE", "{\"mode\":\"deep_focus\",\"source\":\"mqtt\"}");
      } else if (strcmp(mode, "light_work") == 0) {
        timerManager.enterFocus(STATE_LIGHT_WORK, false);
        uiInvalidate();
        logEvent("MQTT_MODE", "{\"mode\":\"light_work\",\"source\":\"mqtt\"}");
      } else if (strcmp(mode, "study") == 0) {
        timerManager.enterFocus(STATE_STUDY, false);
        uiInvalidate();
        logEvent("MQTT_MODE", "{\"mode\":\"study\",\"source\":\"mqtt\"}");
      } else if (strcmp(mode, "standby") == 0) {
        timerManager.enterStandby();
        uiInvalidate();
        logEvent("MQTT_MODE", "{\"mode\":\"standby\",\"source\":\"mqtt\"}");
      } else if (strcmp(mode, "pause") == 0) {
        timerManager.enterPause();
        uiInvalidate();
        logEvent("MQTT_MODE", "{\"mode\":\"pause\",\"source\":\"mqtt\"}");
      }
    }
  }
  else if (strcmp(cmdName, "set_mute") == 0) {
    bool val;
    if (getBoolField("value", val)) {
      keys.setMuted(val);
      audio.setMuted(val);
      logEvent("MUTE", val ? "{\"muted\":true}" : "{\"muted\":false}");
    }
  }
  else if (strcmp(cmdName, "set_lock") == 0) {
    bool val;
    if (getBoolField("value", val)) {
      keys.setLocked(val);
      uiSetLocked(val);
      uiInvalidate();
      logEvent("LOCK", val ? "{\"locked\":true,\"trigger\":\"mqtt\"}" : "{\"locked\":false,\"trigger\":\"mqtt\"}");
    }
  }
  else if (strcmp(cmdName, "set_brightness") == 0) {
    long val;
    if (getIntField("value", val)) {
      if (val < 0) val = 0;
      if (val > 100) val = 100;
      display.setBrightness((uint8_t)val);
      char detail[32];
      snprintf(detail, sizeof(detail), "{\"val\":%ld}", val);
      logEvent("BRIGHTNESS", detail);
    }
  }
  else if (strcmp(cmdName, "set_volume") == 0) {
    long val;
    if (getIntField("value", val)) {
      if (val < 0) val = 0;
      if (val > 255) val = 255;
      audio.setVolume((uint8_t)val);
      char detail[32];
      snprintf(detail, sizeof(detail), "{\"val\":%ld}", val);
      logEvent("VOLUME", detail);
    }
  }
  else if (strcmp(cmdName, "beep") == 0) {
    char tone[32] = {0};
    if (getStrField("tone", tone, sizeof(tone))) {
      if (strcmp(tone, "key_tick") == 0) audio.playTone(AUDIO_KEY_TICK);
      else if (strcmp(tone, "focus_start") == 0) audio.playTone(AUDIO_FOCUS_START);
      else if (strcmp(tone, "pause") == 0) audio.playTone(AUDIO_PAUSE);
      else if (strcmp(tone, "resume") == 0) audio.playTone(AUDIO_RESUME);
      else if (strcmp(tone, "cycle_end") == 0) audio.playTone(AUDIO_CYCLE_END);
      else if (strcmp(tone, "rest_start") == 0) audio.playTone(AUDIO_REST_START);
      else if (strcmp(tone, "pose_confirm") == 0) audio.playTone(AUDIO_POSE_CONFIRM);
      else audio.playTone(AUDIO_KEY_TICK);
    } else {
      audio.playTone(AUDIO_KEY_TICK);
    }
  }
  else if (strcmp(cmdName, "sync_time") == 0) {
    long ts;
    if (getIntField("ts", ts)) {
      time_t t = (time_t)ts;
      struct tm *tm_info = localtime(&t);
      if (tm_info) {
        /* RTC 统一存储本地时间（CST），与 NTP 路径和读路径保持一致 */
        storage.setRtcDateTime(
          tm_info->tm_year + 1900,
          (uint8_t)(tm_info->tm_mon + 1),
          (uint8_t)tm_info->tm_mday,
          (uint8_t)tm_info->tm_hour,
          (uint8_t)tm_info->tm_min,
          (uint8_t)tm_info->tm_sec
        );
        struct timeval tv;
        tv.tv_sec = t;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        logEvent("MQTT_SYNC_TIME", nullptr);
      }
    }
  }
  else if (strcmp(cmdName, "info") == 0) {
    // 查询设备信息，上行回复到 info 主题
    char reply[256];
    snprintf(reply, sizeof(reply),
      "{\"firmware\":\"%s\",\"state\":\"%s\",\"battery\":%d,\"mute\":%s,\"locked\":%s}",
      FIRMWARE_VERSION, timerManager.stateNameEn(),
      powerManager.getBatteryPercent(),
      keys.isMuted() ? "true" : "false",
      keys.isLocked() ? "true" : "false");
    network.publishInfo(reply);
  }
}

// ==================== App 全局状态 ====================
static unsigned long lastPoseMs = 0;
static unsigned long lastTimerMs = 0;
static unsigned long lastScreenMs = 0;
static unsigned long lastLowBatCheckMs = 0;
static unsigned long lastLoopMs = 0;
static uint32_t emotionTimeoutSec = 10;        // 情绪选择倒计时（秒）。uint32_t 避免 config 传入 >65535s 时截断
static unsigned long emotionStartMs = 0;       // 情绪选择开始时刻（进入S8时设置）
static bool emotionRunning = false;            // 情绪倒计时是否在运行
static unsigned long lastScreenOnMs = 0;       // 最后一次亮屏时刻（TASK-A4 功耗管理用）
static unsigned long lastPowerManageMs = 0;    // 功耗评估计时
static bool s_pendingScreenSwitch = false;    // 延迟屏幕切换（避免 LVGL 渲染阻塞主循环）
// 串口调试控制台事件注入（debug_console.h）
volatile KeyEvent debug_inject_key = KEY_EVT_NONE;
volatile PoseFace debug_inject_pose = POSE_UNKNOWN;
volatile bool debug_inject_pose_stable = false;

// 姿态名字（调试用）
static const char* POSE_NAMES[] = {
  "FLAT_UP", "FLAT_DOWN", "UPRIGHT", "LEFT", "RIGHT", "INVERTED", "UNKNOWN"
};

// ==================== 事件记录（数据契约 v1.0 标准 JSON） ====================
// 状态枚举 → 英文模式字符串
static const char* modeStr(SystemState s) {
  switch (s) {
    case STATE_STANDBY:       return "standby";
    case STATE_DEEP_FOCUS:    return "deep_focus";
    case STATE_LIGHT_WORK:    return "light_work";
    case STATE_STUDY:         return "study";
    case STATE_DEEP_REST:     return "deep_rest";
    case STATE_LIGHT_REST:    return "light_rest";
    case STATE_STUDY_REST:    return "study_rest";
    case STATE_PAUSE:         return "pause";
    case STATE_EMOTION_PICK:  return "emotion_select";
    default:                  return "unknown";
  }
}

// action 旧名 → 数据契约 evt 标准名
static const char* eventType(const char *action) {
  static const char* const table[][2] = {
    {"BOOT", "device_boot"},             {"START", "focus_start"},
    {"PAUSE", "focus_pause"},            {"RESUME", "focus_resume"},
    {"MODE_SWITCH", "focus_switch"},     {"CYCLE_END", "focus_end"},
    {"EMOTION", "emotion_select"},       {"REST_END", "rest_end"},
    {"REST_ENDED", "rest_end"},          {"REST_ENDED_AUTO", "rest_end"},
    {"REST_ENDED_RESUME", "focus_resume"},
    {"REST_ENDED新模式", "focus_resume"}, {"REST_MODE_SWITCH", "rest_switch"},
    {"LOCK", "lock_change"},             {"KEY", "key_press"},
    {"MUTE", "mute_change"},             {"WAKE", "wake"},
    {"LOWBAT_WARN", "battery_warn"},     {"LOWBAT_CRIT", "battery_crit"},
    {"POWER_L1", "power_l1"},            {"POWER_L2", "power_l2"},
    {"POWER_L3", "power_l3"},            {"POWER_L3_EXIT", "power_l3_exit"},
    {"POWER_L0", "power_off"},
    {"BRIGHTNESS", "config_change"},     {"VOLUME", "config_change"},
    {"MQTT_MODE", "mqtt_command"},       {"MQTT_SYNC_TIME", "mqtt_command"},
  };
  for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
    if (strcmp(table[i][0], action) == 0) return table[i][1];
  }
  return action;
}

// 记录事件：输出 {"v":1,"ts":...,"dev":"...","evt":"...","data":{...}}
// dataJson 必须是以 '{' 开头的合法 JSON 对象，或 nullptr（用 {} 代替）
static void logEvent(const char *action, const char *dataJson = nullptr) {
  char buf[480];
  time_t now = time(nullptr);
  const char *evt = eventType(action);
  const char *dev = DEVICE_ID_DEFAULT;
  const char *data = dataJson ? dataJson : "{}";
  snprintf(buf, sizeof(buf),
           "{\"v\":1,\"ts\":%lu,\"dev\":\"%s\",\"evt\":\"%s\",\"data\":%s}",
           (unsigned long)now, dev, evt, data);
  Serial.print("[EVT] ");
  Serial.println(buf);
  network.reportEvent(buf);

  // 关键事件立即标记刷盘（不阻塞，只设标志）
  if (strcmp(action, "BOOT") == 0 ||
      strcmp(action, "START") == 0 ||
      strcmp(action, "CYCLE_END") == 0 ||
      strcmp(action, "LOCK") == 0 ||
      strcmp(action, "POWER_L3") == 0) {
    storage.forceFlush();
  }
}

// ==================== 姿态处理子函数 ====================
static SystemState poseToWorkState(PoseFace f) {
  if (f == POSE_UPRIGHT) return STATE_DEEP_FOCUS;
  if (f == POSE_LEFT)    return STATE_LIGHT_WORK;
  if (f == POSE_RIGHT)   return STATE_STUDY;
  return STATE_STANDBY;
}
static bool isWorkPose(PoseFace f) {
  return f == POSE_UPRIGHT || f == POSE_LEFT || f == POSE_RIGHT;
}
static bool isWorkState(SystemState s) {
  return s == STATE_DEEP_FOCUS || s == STATE_LIGHT_WORK || s == STATE_STUDY;
}
static bool isRestState(SystemState s) {
  return s == STATE_DEEP_REST || s == STATE_LIGHT_REST || s == STATE_STUDY_REST;
}

static void handleStandbyPose(PoseFace newFace) {
  if (newFace == POSE_FLAT_DOWN) {
    uiSetScreenOn(false);
    return;
  }
  if (isWorkPose(newFace)) {
    uiSetScreenOn(true);
    lastScreenOnMs = millis();
    SystemState ns = poseToWorkState(newFace);
    timerManager.enterFocus(ns, true);
    uiInvalidate();
    { char _d[128]; snprintf(_d,sizeof(_d),
      "{\"mode\":\"%s\",\"duration_plan\":%lu,\"prev_state\":\"standby\"}",
      modeStr(ns), (unsigned long)timerManager.getCycleTotal());
      logEvent("START", _d); }
    audio.playTone(AUDIO_FOCUS_START);
  }
}

static void handleWorkPose(PoseFace newFace) {
  if (newFace == POSE_FLAT_UP) {
    SystemState prevMode = timerManager.getState();
    unsigned long elapsed = timerManager.getCurrentDuration();
    timerManager.enterPause();
    uiInvalidate();
    { char _d[128]; snprintf(_d,sizeof(_d),
      "{\"mode\":\"%s\",\"elapsed\":%lu,\"cause\":\"flat_up\"}",
      modeStr(prevMode), (unsigned long)elapsed);
      logEvent("PAUSE", _d); }
    audio.playTone(AUDIO_PAUSE);
  } else if (newFace == POSE_FLAT_DOWN) {
    SystemState prevMode = timerManager.getState();
    unsigned long durActual = timerManager.getCurrentDuration();
    unsigned long durPlan = timerManager.getCycleTotal();
    timerManager.enterStandby();
    s_pendingScreenSwitch = true;
    { char _d[160]; snprintf(_d,sizeof(_d),
      "{\"cause\":\"facedown\",\"mode\":\"%s\",\"duration_actual\":%lu,\"duration_plan\":%lu,\"effective\":false}",
      modeStr(prevMode), (unsigned long)durActual, (unsigned long)durPlan);
      logEvent("CYCLE_END", _d); }
    audio.playTone(AUDIO_CYCLE_END);
  } else if (isWorkPose(newFace)) {
    SystemState fromMode = timerManager.getState();
    SystemState ns = poseToWorkState(newFace);
    if (ns != fromMode) {
      unsigned long elapsed = timerManager.getCurrentDuration();
      timerManager.enterFocus(ns, true);
      uiInvalidate();
      { char _d[160]; snprintf(_d,sizeof(_d),
        "{\"from_mode\":\"%s\",\"to_mode\":\"%s\",\"elapsed\":%lu,\"duration_plan\":%lu}",
        modeStr(fromMode), modeStr(ns), (unsigned long)elapsed, (unsigned long)timerManager.getCycleTotal());
        logEvent("MODE_SWITCH", _d); }
      audio.playTone(AUDIO_POSE_CONFIRM);
    }
  }
}

static void handlePausePose(PoseFace newFace) {
  if (isWorkPose(newFace)) {
    SystemState newState = poseToWorkState(newFace);
    if (newState == timerManager.getPreviousWorkState()) {
      unsigned long pauseDur = timerManager.getPauseDuration();
      unsigned long total = timerManager.getCycleTotal();
      unsigned long elapsed = timerManager.getSavedWorkDuration();
      timerManager.enterFocus(newState, false);
      { char _d[128]; snprintf(_d,sizeof(_d),
        "{\"mode\":\"%s\",\"remaining\":%lu,\"pause_duration\":%lu}",
        modeStr(newState), (unsigned long)(total > elapsed ? total - elapsed : 0), (unsigned long)pauseDur);
        logEvent("RESUME", _d); }
      audio.playTone(AUDIO_RESUME);
    } else {
      SystemState fromMode = timerManager.getPreviousWorkState();
      timerManager.commitPauseWorkAsIncomplete();
      timerManager.enterFocus(newState, true);
      { char _d[160]; snprintf(_d,sizeof(_d),
        "{\"from_mode\":\"%s\",\"to_mode\":\"%s\",\"elapsed\":0,\"duration_plan\":%lu}",
        modeStr(fromMode), modeStr(newState), (unsigned long)timerManager.getCycleTotal());
        logEvent("MODE_SWITCH", _d); }
      audio.playTone(AUDIO_POSE_CONFIRM);
    }
    uiInvalidate();
  } else if (newFace == POSE_FLAT_DOWN) {
    SystemState prevMode = timerManager.getPreviousWorkState();
    unsigned long durActual = timerManager.getSavedWorkDuration();
    unsigned long durPlan = timerManager.getCycleTotal();
    timerManager.enterStandby();
    s_pendingScreenSwitch = true;
    { char _d[160]; snprintf(_d,sizeof(_d),
      "{\"cause\":\"facedown_from_pause\",\"mode\":\"%s\",\"duration_actual\":%lu,\"duration_plan\":%lu,\"effective\":false}",
      modeStr(prevMode), (unsigned long)durActual, (unsigned long)durPlan);
      logEvent("CYCLE_END", _d); }
    audio.playTone(AUDIO_CYCLE_END);
  }
}

static void handleRestPose(PoseFace newFace) {
  if (newFace == POSE_FLAT_UP) return;
  if (newFace == POSE_FLAT_DOWN) {
    SystemState prevMode = timerManager.getState();
    timerManager.enterStandby();
    s_pendingScreenSwitch = true;
    { char _d[128]; snprintf(_d,sizeof(_d),
      "{\"mode\":\"%s\",\"remaining\":0,\"skipped\":false,\"cause\":\"facedown\"}",
      modeStr(prevMode));
      logEvent("REST_END", _d); }
    audio.playTone(AUDIO_REST_END);
  } else if (isWorkPose(newFace)) {
    SystemState ns = poseToWorkState(newFace);
    if (timerManager.isRestEnded()) {
      timerManager.enterFocus(ns, true);
      { char _d[128]; snprintf(_d,sizeof(_d),
        "{\"mode\":\"%s\",\"duration_plan\":%lu,\"prev_state\":\"rest\"}",
        modeStr(ns), (unsigned long)timerManager.getCycleTotal());
        logEvent("REST_ENDED新模式", _d); }
    } else {
      timerManager.enterFocus(ns, true);
      { char _d[128]; snprintf(_d,sizeof(_d),
        "{\"mode\":\"%s\",\"remaining\":0,\"skipped\":true}",
        modeStr(ns));
        logEvent("REST_MODE_SWITCH", _d); }
    }
    uiInvalidate();
    audio.playTone(AUDIO_POSE_CONFIRM);
  }
}

static void onPoseStable(PoseFace newFace) {
  SystemState cur = timerManager.getState();

  if (keys.isLocked()) {
    if (newFace == POSE_FLAT_DOWN) {
      uiSetScreenOn(false);
    }
    return;
  }
  if (cur == STATE_EMOTION_PICK) {
    if (newFace == POSE_FLAT_DOWN) {
      uiSetScreenOn(false);
    }
    return;
  }

  /* 姿态变化时亮屏：非反扣且屏熄就点亮；从反扣翻起也强制点亮 */
  if (newFace != POSE_FLAT_DOWN) {
    if (!uiIsScreenOn()) {
      uiSetScreenOn(true);
      lastScreenOnMs = millis();
    }
  }

  // 屏幕旋转跟随姿态（MADCTL 寄存器，LVGL 无需感知）
  if (newFace == POSE_UPRIGHT) {
    if (display.getRotation() != 0) {
      display.setRotation(0);
      touch.setRotation(0);
    }
  } else if (newFace == POSE_LEFT) {
    if (display.getRotation() != 90) {
      display.setRotation(90);
      touch.setRotation(90);
    }
  } else if (newFace == POSE_RIGHT) {
    if (display.getRotation() != 270) {
      display.setRotation(270);
      touch.setRotation(270);
    }
  }
  /* 旋转分支不调 uiInvalidate()：MADCTL 改物理像素映射，
   * LVGL 仍渲染到同一坐标系，物理旋转对 LVGL 透明。
   * 若姿态变化引发状态切换，由下面的状态分支调 uiInvalidate()。 */

  if (cur == STATE_STANDBY)      { handleStandbyPose(newFace); return; }
  if (isWorkState(cur))          { handleWorkPose(newFace);    return; }
  if (cur == STATE_PAUSE)        { handlePausePose(newFace);  return; }
  if (isRestState(cur))          { handleRestPose(newFace);   return; }
}

static void onEmotionConfirm(uint8_t idx) {
  static const char *emoLabels[4] = { "flow", "stuck", "plain", "drained" };
  if (idx >= 4) { /* 防御性检查：LVGL 回调可能传越界值（UB → 崩溃） */
    Serial.printf("[EMO] invalid idx=%u, ignoring\n", idx);
    return;
  }
  bool isTimeout = s_emotionTimeout;
  s_emotionTimeout = false;  // 消费标记，不影响下次
#ifdef DEBUG_SERIAL
  Serial.printf("[EMO] confirm idx=%u label=%s timeout=%d dur=%lu hadPause=%d\n",
    idx, emoLabels[idx], isTimeout,
    (unsigned long)timerManager.getSavedWorkDuration(),
    timerManager.getHadPause());
#endif
  char detail[96];
  snprintf(detail, sizeof(detail), "{\"emotion\":\"%s\",\"auto\":%s,\"prev_mode\":\"%s\"}",
           emoLabels[idx], isTimeout ? "true" : "false", modeStr(timerManager.getPreviousWorkState()));
  logEvent("EMOTION", detail);

  emotionRunning = false;

  // v5.3 有效专注判定：
  //   高效 = 完整周期 + 无暂停(S4) + 情绪选"顺畅/平淡" 且非超时
  //   否则一律低效
  unsigned long dur = timerManager.getSavedWorkDuration();
  if (dur > 0) {
    bool ineffective = isTimeout                          // 超时 → 无效
                    || timerManager.getHadPause()          // 有暂停 → 无效
                    || (idx == 1 || idx == 3);             // 卡壳/疲惫 → 无效
    if (ineffective) {
      timerManager.addIneffectiveToday(dur);
    } else {
      timerManager.addEffectiveToday(dur);
    }
  }

  SystemState prev = timerManager.getPreviousWorkState();
  // v5.2.0: 只有完整周期才进S8，选完直接进休息
  timerManager.enterRest(prev);
  uiInvalidate();
  audio.playTone(AUDIO_REST_START);
  uiHideTotalPopup();
}

// ==================== 触屏处理 ====================
// S8 情绪选择 2×2 网格热区（480×480 屏幕）
// 视觉布局：左上=顺畅 右上=平淡 / 左下=卡壳 右下=耗竭
// 按钮尺寸 186×90，起始 x=48, y=160，间距 h=28, v=40
#define EMOTION_ROW0_Y    160   // 第一行 Y 起点
#define EMOTION_ROW0_END  250   // 第一行 Y 终点
#define EMOTION_ROW1_Y    290   // 第二行 Y 起点
#define EMOTION_ROW1_END  380   // 第二行 Y 终点
#define EMOTION_COL0_X    48    // 左列 X 起点
#define EMOTION_COL0_END  234   // 左列 X 终点
#define EMOTION_COL1_X    262   // 右列 X 起点
#define EMOTION_COL1_END  448   // 右列 X 终点

static void checkTouch() {
  uint16_t x, y;
  if (!touch.read(&x, &y)) return;

  // 锁定时：触屏只亮屏，不响应任何操作
  if (keys.isLocked()) {
    if (!uiIsScreenOn()) {
      uiSetScreenOn(true);
      lastScreenOnMs = millis();
    } else {
      uiResetScreenTimer();
      lastScreenOnMs = millis();
    }
    return;
  }

  uiResetScreenTimer();
  lastScreenOnMs = millis();

  bool wokeFromL2 = false;
  if (powerManager.getPowerLevel() >= POWER_L2_DEEP_SLEEP) {
    powerManager.setPowerLevel(POWER_L0_NORMAL);
    wokeFromL2 = true;
    logEvent("WAKE", "{\"src\":\"touch\"}");
  }

  /* 唤醒时 / 熄屏时任意触摸强制亮屏 — 必须放在 S8 return 分支之前 */
  if (wokeFromL2 || !uiIsScreenOn()) {
    uiSetScreenOn(true);
    lastScreenOnMs = millis();
  }

  SystemState s = timerManager.getState();

  // S8 情绪选择：LVGL 路径由 button callback 处理，原生路径用手势坐标
  if (s == STATE_EMOTION_PICK) {
#ifdef USE_LVGL
    /* LVGL 按钮回调已注册（ui_set_emotion_callback），不重复处理 */
    return;
#else
    if (y >= EMOTION_ROW0_Y && y < EMOTION_ROW0_END) {
      if (x >= EMOTION_COL0_X  && x < EMOTION_COL0_END) onEmotionConfirm(0);  // 顺畅
      if (x >= EMOTION_COL1_X && x < EMOTION_COL1_END) onEmotionConfirm(2);  // 平淡
    } else if (y >= EMOTION_ROW1_Y && y < EMOTION_ROW1_END) {
      if (x >= EMOTION_COL0_X  && x < EMOTION_COL0_END) onEmotionConfirm(1);  // 卡壳
      if (x >= EMOTION_COL1_X && x < EMOTION_COL1_END) onEmotionConfirm(3);  // 耗竭
    }
    return;
#endif
  }

  // 休息结束提示页：触摸 → 同模式新周期
  if (timerManager.isRestEnded()) {
    SystemState prev = timerManager.getPreviousWorkState();
    timerManager.enterFocus(prev, true);
    uiInvalidate();
    { char _d[64]; snprintf(_d,sizeof(_d),"{\"mode\":\"%s\"}",modeStr(prev)); logEvent("REST_ENDED_RESUME", _d); }
    audio.playTone(AUDIO_FOCUS_START);
  }
}

// ==================== 按键处理 ====================
static void onKey(KeyEvent ev) {
  // 锁定时：BOOT 长按（解锁）有效，其余忽略
  if (keys.isLocked()) {
    if (ev == KEY_EVT_BOOT_LONG) {
      if (powerManager.getPowerLevel() >= POWER_L2_DEEP_SLEEP) {
        powerManager.setPowerLevel(POWER_L0_NORMAL);
        logEvent("WAKE", "{\"src\":\"key_locked\"}");
      }
      keys.setLocked(false);
      uiSetLocked(false);
      uiResetScreenTimer();
      lastScreenOnMs = millis();
      /* 解锁时强制亮屏 — 即使从 L2 深度休眠唤醒也要点亮，否则 lvglBridge_showScreenForState 会因屏熄跳过切屏 */
      uiSetScreenOn(true);

      /* 解锁后姿态一致性检查：避免锁定期间姿态变化导致状态不一致 */
      {
        PoseFace curFace = poseDetector.getFace();
        SystemState curState = timerManager.getState();

        if (curFace == POSE_FLAT_DOWN) {
          /* 反扣 → 直接熄屏待机（用户可能是反扣着解锁的） */
          timerManager.enterStandby();
          uiInvalidate();
          uiSetScreenOn(false);
          logEvent("LOCK", "{\"locked\":false,\"trigger\":\"boot_long\",\"pose_correction\":\"flat_down→standby\"}");
          audio.playTone(AUDIO_UNLOCK);
          return;
        }

        if (curState == STATE_STANDBY) {
          /* 待机状态：如果当前是工作姿态，保持待机（用户主动待机的，不自动进入专注） */
        } else if (isWorkState(curState) || curState == STATE_PAUSE) {
          /* 专注/暂停状态：检查当前姿态是否匹配 */
          if (curFace == POSE_FLAT_UP) {
            /* 平放 → 应该是暂停状态 */
            if (curState != STATE_PAUSE) {
              SystemState prevMode = curState;
              timerManager.enterPause();
              { char _d[128]; snprintf(_d, sizeof(_d),
                "{\"locked\":false,\"trigger\":\"boot_long\",\"pose_correction\":\"work→pause\",\"prev_mode\":\"%s\"}",
                modeStr(prevMode));
                logEvent("LOCK", _d); }
            }
          } else if (isWorkPose(curFace)) {
            /* 工作姿态 → 应该是专注状态 */
            SystemState expectedState = poseToWorkState(curFace);
            if (curState == STATE_PAUSE) {
              /* 从暂停恢复 */
              unsigned long pauseDur = timerManager.getPauseDuration();
              unsigned long total = timerManager.getCycleTotal();
              unsigned long elapsed = timerManager.getSavedWorkDuration();
              if (expectedState == timerManager.getPreviousWorkState()) {
                /* 同模式恢复 */
                timerManager.enterFocus(expectedState, false);
                { char _d[128]; snprintf(_d, sizeof(_d),
                  "{\"locked\":false,\"trigger\":\"boot_long\",\"pose_correction\":\"pause→resume\",\"mode\":\"%s\",\"pause_dur\":%lu}",
                  modeStr(expectedState), (unsigned long)pauseDur);
                  logEvent("LOCK", _d); }
              } else {
                /* 不同模式，切换新周期 */
                timerManager.commitPauseWorkAsIncomplete();
                timerManager.enterFocus(expectedState, true);
                { char _d[160]; snprintf(_d, sizeof(_d),
                  "{\"locked\":false,\"trigger\":\"boot_long\",\"pose_correction\":\"pause→new_mode\",\"from\":\"%s\",\"to\":\"%s\"}",
                  modeStr(timerManager.getPreviousWorkState()), modeStr(expectedState));
                  logEvent("LOCK", _d); }
              }
            } else if (curState != expectedState) {
              /* 当前是其他工作模式，切换到姿态对应的模式 */
              SystemState fromMode = curState;
              timerManager.enterFocus(expectedState, true);
              { char _d[160]; snprintf(_d, sizeof(_d),
                "{\"locked\":false,\"trigger\":\"boot_long\",\"pose_correction\":\"mode_switch\",\"from\":\"%s\",\"to\":\"%s\"}",
                modeStr(fromMode), modeStr(expectedState));
                logEvent("LOCK", _d); }
            }
          }
        } else if (isRestState(curState)) {
          /* 休息状态：姿态不影响休息，保持原状 */
        } else if (curState == STATE_EMOTION_PICK) {
          /* 情绪选择状态：保持原状，等用户选择或超时 */
        }
      }

      uiInvalidate();
      logEvent("LOCK", "{\"locked\":false,\"trigger\":\"boot_long\"}");
      audio.playTone(AUDIO_UNLOCK);
    }
    return;
  }

  uiResetScreenTimer();
  lastScreenOnMs = millis();

  bool keyWokeFromL2 = false;
  if (powerManager.getPowerLevel() >= POWER_L2_DEEP_SLEEP) {
    powerManager.setPowerLevel(POWER_L0_NORMAL);
    keyWokeFromL2 = true;
    logEvent("WAKE", "{\"src\":\"key\"}");
  }

  /* 从 L2 唤醒时 / 熄屏状态下按键 — 强制亮屏 */
  if (keyWokeFromL2 || !uiIsScreenOn()) {
    uiSetScreenOn(true);
    lastScreenOnMs = millis();
  }

  switch (ev) {
    case KEY_EVT_BOOT_CLICK:
      audio.playTone(AUDIO_KEY_TICK);
      { char _d[96]; snprintf(_d, sizeof(_d),
        "{\"key\":\"boot\",\"press\":\"click\",\"state\":\"%s\"}",
        modeStr(timerManager.getState()));
        logEvent("KEY", _d); }
      break;
    case KEY_EVT_BOOT_LONG:
      keys.setLocked(true);
      uiSetLocked(true);
      logEvent("LOCK", "{\"locked\":true,\"trigger\":\"boot_long\"}");
      audio.playTone(AUDIO_LOCK);
      break;
    case KEY_EVT_USER_CLICK: {
      audio.playTone(AUDIO_KEY_TICK);
      uiShowTotalPopup();
      uiSetScreenOn(true);            /* 弹窗前确保物理屏点亮，否则黑屏上看不到统计界面 */
      lastScreenOnMs = millis();
      network.requestConnect();
      network.requestMqttReconnect();
      { char _d[96]; snprintf(_d, sizeof(_d),
        "{\"key\":\"user\",\"press\":\"click\",\"state\":\"%s\"}",
        modeStr(timerManager.getState()));
        logEvent("KEY", _d); }
      break;
    }
    case KEY_EVT_USER_LONG: {
      // 静音切换
      keys.toggleMute();
      bool muted = keys.isMuted();
      audio.setMuted(muted);
      logEvent("MUTE", muted ? "{\"muted\":true}" : "{\"muted\":false}");
      break;
    }
    default: break;
  }
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  // 等待 USB CDC 枚举完成（ESP32-C6 USB Serial/JTAG 需要时间建立双向通信）
  unsigned long serWaitStart = millis();
  while (!Serial && millis() - serWaitStart < 3000) { delay(10); }
  delay(200);
  Serial.printf("\n[BOOT] ChronoCube v%s start (UI: %s)\n", FIRMWARE_VERSION,
#ifdef USE_LVGL
    "LVGL 9.5"
#else
    "Native"
#endif
  );
  Serial.flush();

  // I2C
  I2CBus::begin(I2C_SDA, I2C_SCL, 400000);
  Serial.println("[BOOT] I2C OK");

  // PMU
  if (!powerManager.begin()) {
    Serial.println("[WARN] PMU init failed — battery/backlight may be unavailable");
  } else {
    Serial.println("[BOOT] PMU OK");
  }

  // 显示
  if (!display.begin()) {
    Serial.println("[FATAL] display init failed — HALT");
    while (1) { delay(1000); }  // P1-5: 显示失败后死循环，不允许带病运行
  } else {
    Serial.println("[BOOT] display OK");
    display.setBrightness(configGetRuntime().defaultBrightness);
  }
  // 注册 L2→L0 唤醒钩子：反扣进入深度休眠后唤醒需重初始化 LCD（见 display.cpp reinitAfterWake）
  powerManager.setWakeCallback([]() { display.reinitAfterWake(); });

  // 触控
  touch.begin();
  Serial.println("[BOOT] touch OK");

  // 姿态传感器
  if (!poseDetector.begin()) {
    Serial.println("[FATAL] QMI8658 init failed");
  } else {
    Serial.println("[BOOT] pose OK");
  }

  // 音频
  if (!audio.begin()) {
    Serial.println("[WARN] audio init failed");
  } else {
    Serial.println("[BOOT] audio OK");
  }

  // SD 卡（与 LCD QSPI 共享 SPI2_HOST，通过不同 CS 区分）
  if (!storage.begin()) {
    Serial.println("[WARN] SD card init failed, storage unavailable");
  } else {
    Serial.println("[BOOT] SD card OK");
    /* P0: SPI2 总线互斥锁 — SD + LCD 就绪后初始化，保护后续所有 SPI2 操作 */
    // P1-5: 即使 SD 失败也初始化锁（M1 fix），避免其他 SPI2 操作在未初始化的锁上阻塞
  }
  spi2_lock_init();  // P1-5: 无条件初始化，LCD/SD 均可使用
  Serial.println("[BOOT] SPI2 mutex OK");
  if (storage.isReady()) {
#ifndef USE_LVGL
    // 字库加载：仅原生 UI 路径使用 fontLoader（LVGL 路径走 font_adapter）
    bool cnOk = fontLoader.beginCN("/sdcard/ChronoCube/fonts/cn_24.bin");
    bool enOk = fontLoader.beginEN("/sdcard/ChronoCube/fonts/en_24.bin");
    if (!cnOk || !enOk) {
      Serial.printf("[WARN] font loader partial: CN=%s EN=%s\n",
        cnOk ? "OK" : "FAIL", enOk ? "OK" : "FAIL");
    } else {
      Serial.println("[BOOT] font loader OK (CN 24x24 + EN 12x24)");
    }
#endif
  }

  // RTC → 系统时间同步（让 time(nullptr)/localtime() 返回真实时间）
  {
    RtcDateTime rtcDt;
    if (storage.getRtcDateTime(rtcDt)) {
      // 合理性检查：只接受 2020-9999 年范围，拒绝出厂随机值/电池耗尽
      if (rtcDt.year >= 2020 && rtcDt.year <= 9999 &&
          rtcDt.month >= 1 && rtcDt.month <= 12 &&
          rtcDt.day >= 1 && rtcDt.day <= 31) {
        setenv("TZ", "CST-8", 1);   // 中国标准时间 UTC+8
        tzset();

        struct tm t = {};
        t.tm_year = rtcDt.year - 1900;
        t.tm_mon  = rtcDt.month - 1;
        t.tm_mday = rtcDt.day;
        t.tm_hour = rtcDt.hour;
        t.tm_min  = rtcDt.minute;
        t.tm_sec  = rtcDt.second;
        time_t now = mktime(&t);

        struct timeval tv;
        tv.tv_sec  = now;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);

        Serial.printf("[RTC] system time synced: %04d-%02d-%02d %02d:%02d:%02d CST\n",
                      rtcDt.year, rtcDt.month, rtcDt.day,
                      rtcDt.hour, rtcDt.minute, rtcDt.second);
      } else {
        Serial.printf("[RTC] time invalid (%04d-%02d-%02d), skipping sync\n",
                      rtcDt.year, rtcDt.month, rtcDt.day);
      }
    } else {
      Serial.println("[RTC] no valid time — lock date will show compile fallback");
    }
  }

  // 运行时配置：SD 卡覆盖编译期默认值（P0: 修复 configLoaderBegin 从未调用）
  if (storage.isReady()) {
    configLoaderBegin();
    // P1: 将运行时加载的姿态参数同步到 PoseDetector（此前仅加载未应用 → 死代码）
    const RuntimeConfig &cfg = configGetRuntime();
    poseDetector.setAngleThreshold(cfg.poseAngleThreshold);
    poseDetector.setConfirmMs(cfg.poseConfirmMs);
    poseDetector.setFacedownConfirmMs(cfg.poseFacedownConfirmMs);
    poseDetector.setFlipFastMs(cfg.poseFlipFastMs);
    poseDetector.setPredelayMs(cfg.posePredelayMs);
    poseDetector.setGyroFlipDps(cfg.poseGyroFlipDps);
    poseDetector.setGyroStillDps(cfg.poseGyroStillDps);
    poseDetector.setMotionFilterG(cfg.poseMotionFilterG);
    // 同步运行时配置到计时器/按键/情绪模块
    timerManager.applyRuntimeConfig(cfg.deepFocusWork, cfg.deepFocusRest,
                                     cfg.lightWorkWork, cfg.lightWorkRest,
                                     cfg.studyWork, cfg.studyRest,
                                     cfg.ineffectiveRatio);
    emotionTimeoutSec = cfg.emotionTimeout / 1000;  // 去掉 uint16_t 强转，避免 >65535 秒时截断（emotionTimeoutSec 本身为 uint32_t）
    // 同步运行时配置到显示亮度（display.begin 时配置未加载，此处二次应用确保生效）
    display.setBrightness(cfg.defaultBrightness);
  }

  // 按键
  keys.begin();
  if (storage.isReady()) {
    const RuntimeConfig &cfg = configGetRuntime();
    keys.setLongpressMs(cfg.keyLongpress);
  }
  Serial.println("[BOOT] keys OK");

  // 计时器
  timerManager.begin();
  Serial.println("[BOOT] timer OK");
  Serial.flush();

  // UI（提前到 Network 之前：LVGL 需 38KB DMA，WiFi 会吃 ~60KB，
  //     必须在 WiFi 分配前抢占 DMA 池，否则 OOM → 绿屏）
  Serial.println("[BOOT] UI begin...");
  Serial.flush();
  uiBegin();
  if (lv_display_get_default() == NULL) {
    Serial.println("[BOOT] LVGL display init FAILED — screen will not work");
  }
  uiSetScreenOn(true);                         // 初始亮屏
  lastScreenOnMs = millis();
  uiInvalidate();

  // 网络（LVGL DMA 已安全分配后再初始化 WiFi）
  Serial.println("[BOOT] network begin...");
  Serial.flush();
  network.begin();
  network.setCallback(onMqttCommand);
  Serial.println("[BOOT] network OK");
  Serial.flush();

  // RAM 状态检测
  size_t totalRam = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  size_t freeRam = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t usedRam = totalRam - freeRam;
  Serial.printf("[MEM] Total: %d, Free: %d, Used: %d (%.1f%%)\n",
                totalRam, freeRam, usedRam, (float)usedRam / totalRam * 100);

  // 硬件看门狗：8秒超时，panic模式（卡死自动复位）
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 8000,
    .idle_core_mask = 0,      // 监控所有核心
    .trigger_panic = true
  };
  esp_err_t wdtErr = esp_task_wdt_init(&wdt_config);
  if (wdtErr == ESP_OK) {
    esp_task_wdt_add(NULL);  // 监控当前任务（loop任务）
    Serial.println("[BOOT] hardware WDT OK (8s panic)");
  } else {
    Serial.printf("[WARN] hardware WDT init failed: %d\n", wdtErr);
  }

  { char _bd[160]; snprintf(_bd,sizeof(_bd),
    "{\"fw\":\"%s\",\"hw\":\"esp32c6-amoled-2.16\",\"rtc_valid\":%s,\"sd_ready\":%s,\"battery_pct\":%u}",
    FIRMWARE_VERSION,
    storage.isRtcAvailable() ? "true" : "false",
    storage.isReady() ? "true" : "false",
    powerManager.getBatteryPercent());
    logEvent("BOOT", _bd); }
  Serial.println("[BOOT] setup complete\n");
  Serial.flush();
}

// ==================== Loop ====================
void loop() {
  unsigned long now = millis();
  if (now - lastLoopMs < 2) return;
  lastLoopMs = now;

  // 0) Loop 性能记录（在门控之后，统计受控迭代间隔，检测 >5ms / >20ms 阻塞）
  debugLoop_record(now);

  // 0b) 截图 + 串口调试控制台
  debugConsole_tick();
  debugWatchdog_poke("console");

  // 调试事件注入（由 debugConsole_tick 的 key/pose 命令触发）
  if (debug_inject_key != KEY_EVT_NONE) {
    KeyEvent dev = debug_inject_key;
    debug_inject_key = KEY_EVT_NONE;
    onKey(dev);
  }
  if (debug_inject_pose != POSE_UNKNOWN) {
    PoseFace df = debug_inject_pose;
    bool ds = debug_inject_pose_stable;
    debug_inject_pose = POSE_UNKNOWN;
    debug_inject_pose_stable = false;
    if (ds) onPoseStable(df);
  }

  // 1) 按键
  KeyEvent ev = keys.update();
  if (ev != KEY_EVT_NONE) onKey(ev);
  debugWatchdog_poke("keys");

  // 2) 姿态 ~50Hz/10Hz/1Hz 取决于功耗等级（TASK-A4）
  int poseHz = POSE_SAMPLE_HZ;
  PowerLevel pl = powerManager.getPowerLevel();
  if (pl == POWER_L1_STANDBY) poseHz = 10;
  else if (pl >= POWER_L2_DEEP_SLEEP) poseHz = 1;
  if (now - lastPoseMs >= 1000 / poseHz) {
    lastPoseMs = now;
    poseDetector.update();

    // 一阶预反馈（200ms 快速提示，用户可感知）
    if (poseDetector.isPreConfirmed()) {
      poseDetector.clearPreConfirmed();
    }

    // 二阶稳定确认
    if (poseDetector.isStable()) {
      PoseFace f = poseDetector.getFace();
      static PoseFace lastStable = POSE_UNKNOWN;
      if (f != lastStable) {
        lastStable = f;
        Serial.printf("[POSE] face=%s stable=1 dur=%lums ax=%.2f ay=%.2f az=%.2f\n",
          POSE_NAMES[f], poseDetector.getFaceDurationMs(),
          poseDetector.accX, poseDetector.accY, poseDetector.accZ);
        onPoseStable(f);
      }
    }
  }
  debugWatchdog_poke("pose");

  // 3) 计时 1Hz
  if (now - lastTimerMs >= 1000) {
    /* Bug #9: 用 += 1000 而非 = now，避免 loop 延迟导致丢秒累计误差 */
    lastTimerMs += 1000;
    /* 安全阀：如遇极端延迟（>2s），防止雪崩追赶 */
    if (now - lastTimerMs > 2000) lastTimerMs = now - 1000;
    timerManager.tick();

    /* 0 点日清零 */
    {
      time_t tNow = time(nullptr);
      struct tm *tm = localtime(&tNow);
      if (tm && tNow > 1000000000) {
        timerManager.dailyReset(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
      }
    }

    /* LVGL UI 数据更新（每秒同步计时器 → 屏幕） */
    uiUpdateStandbyData();

    /* 屏幕超时自动息屏 */
    {
      SystemState st = timerManager.getState();
      bool keepOn = keys.isLocked() ||
                    (st == STATE_EMOTION_PICK) ||
                    (st == STATE_PAUSE) ||
                    (timerManager.isRestEnded()) ||
                    (lowBatShownMs > 0) ||
                    (totalPopupShownMs > 0);
      if (!keepOn && uiIsScreenOn() && configGetRuntime().powerSaveEnabled) {
        unsigned long onMs = now - lastScreenOnMs;
        unsigned long threshold = (st == STATE_STANDBY)
          ? configGetRuntime().screenOffDelay
          : configGetRuntime().screenFocusOff;
        if (onMs >= threshold) {
          uiSetScreenOn(false);
        }
      }
    }

    /* 专注周期结束处理 → 进入 S8 情绪选择页面 */
    if (timerManager.isCycleEndFlag()) {
      timerManager.clearCycleEndFlag();
      uiInvalidate();  /* 切换到 S8 情绪选择屏幕 */
      uiSetScreenOn(true);
      lastScreenOnMs = millis();
      audio.playTone(AUDIO_CYCLE_END);
      { char _d[96]; snprintf(_d, sizeof(_d),
        "{\"mode\":\"%s\",\"duration\":%lu}",
        modeStr(timerManager.getPreviousWorkState()),
        (unsigned long)timerManager.getSavedWorkDuration());
        logEvent("CYCLE_END", _d); }
    }

    /* 休息结束处理 */
    if (timerManager.isRestEndFlag()) {
      timerManager.clearRestEndFlag();
      /* 非工作姿态（Flat Up 等）→ 用户不在关注设备 → 直接待机 */
      if (!isWorkPose(poseDetector.getFace())) {
        timerManager.enterStandby();
        s_pendingScreenSwitch = true;
        { char _d[64]; snprintf(_d,sizeof(_d),"{\"cause\":\"flat_restend\"}"); logEvent("REST_ENDED", _d); }
        audio.playTone(AUDIO_REST_END);
        return;
      }
      /* 休息结束 → 切换到提示页 + 播放提示音 */
      ui_show_screen(UI_SCREEN_REST_END);
      ui_show_rest_end("即将继续专注 · 5s");
      audio.playTone(AUDIO_REST_END);
      { char _d[64]; snprintf(_d,sizeof(_d),"{\"cause\":\"timer_done\",\"next_mode\":\"%s\"}",
        modeStr(timerManager.getState()));
        logEvent("REST_ENDED", _d); }
    } else if (timerManager.isRestEnded()) {
      /* 休息结束倒计时每秒递减 */
      timerManager.tickRestEndCountdown();
      uint16_t cd = timerManager.getRestEndCountdown();
      ui_set_rest_end_countdown(cd);
      if (cd == 0) {
        /* 5秒无操作 → 回待机，仅节电模式下熄屏 */
        timerManager.enterStandby();
        uiInvalidate();
        if (configGetRuntime().powerSaveEnabled) {
          uiSetScreenOn(false);
        }
        logEvent("REST_ENDED_AUTO", "{\"cause\":\"countdown_expired\"}");
      }
    }

#ifdef USE_LVGL
    /* LVGL 桥接：计时屏数据刷新 */
    uiPushTimerData();
    /* 锁屏时钟每秒刷新 */
    if (ui_get_current_screen() == UI_SCREEN_LOCKED) {
      time_t tNow = time(nullptr);  /* tNow 不和外层 unsigned long now 冲突 */
      struct tm *tm = localtime(&tNow);
      char timeBuf[8];
      if (tm && tNow > 1000000000) {
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tm->tm_hour, tm->tm_min);
      } else {
        snprintf(timeBuf, sizeof(timeBuf), "00:00");
      }
      char dateBuf[32];
      lvglBridge_getLockDateStr(dateBuf, sizeof(dateBuf));
      ui_set_lock_clock(timeBuf, dateBuf);
    }
#endif

    // 低电量分级检测
    if (now - lastLowBatCheckMs >= 10000) {
      lastLowBatCheckMs = now;
      uint8_t bat = powerManager.getBatteryPercent();
      if (bat < configGetRuntime().powerBatteryCritPct) {
        // 低于安全阈值: 强制待机，仅保留 RTC 唤醒
        Serial.printf("[PWR] CRITICAL battery %d%%, force standby\n", bat);
        timerManager.enterStandby();
        uiSetScreenOn(false);
        uiInvalidate();
        { char _d[48]; snprintf(_d,sizeof(_d),"{\"pct\":%u,\"mv\":%u}",bat,
          (uint16_t)(powerManager.getBatteryVoltage() * 1000.0f + 0.5f));
          logEvent("LOWBAT_CRIT", _d); }
      } else if (bat < configGetRuntime().powerBatteryShutPct) {
        // 2%~5%: 低电提示弹窗 2s，允许继续使用
        uiShowLowBattery(bat);
        audio.playTone(AUDIO_LOW_BATTERY);
        { char _d[48]; snprintf(_d,sizeof(_d),"{\"pct\":%u,\"mv\":%u}",bat,
          (uint16_t)(powerManager.getBatteryVoltage() * 1000.0f + 0.5f));
          logEvent("LOWBAT_WARN", _d); }
      }
      // ≥5%: 正常，无提示
    }

    // 情绪选择倒计时（显示用 + 超时处理）
    if (timerManager.getState() == STATE_EMOTION_PICK) {
      if (!emotionRunning) {
        // 刚进入 S8，开始倒计时
        emotionStartMs = now;
        emotionRunning = true;
        uiSetEmotionSec(emotionTimeoutSec);
      } else {
        uint16_t sec = (uint16_t)((now - emotionStartMs) / 1000);
        if (sec >= emotionTimeoutSec) {
          // 超时默认平淡 → v5.3: 超时一律低效
          emotionRunning = false;
          s_emotionTimeout = true;
          onEmotionConfirm(2);
        } else {
          uiSetEmotionSec(emotionTimeoutSec - sec);
        }
      }
    } else {
      emotionRunning = false;
    }
  }
  debugWatchdog_poke("timer");

  // ==================== 功耗自动管理（TASK-A4，每 5s 评估）====================
  if (now - lastPowerManageMs >= 5000) {
    lastPowerManageMs = now;

    uint8_t bat = powerManager.getBatteryPercent();

    // L3: 电量低于关机阈值 → 安全关机
    if (bat < configGetRuntime().powerBatteryShutPct) {
      if (powerManager.getPowerLevel() != POWER_L3_SHUTDOWN) {
        Serial.printf("[PWR] battery=%d%% -> L3 SHUTDOWN\n", bat);
        timerManager.enterStandby();
        uiSetScreenOn(false);
        uiInvalidate();
        powerManager.setPowerLevel(POWER_L3_SHUTDOWN);
        { char _d[48]; snprintf(_d,sizeof(_d),"{\"pct\":%u,\"mv\":%u}",bat,
          (uint16_t)(powerManager.getBatteryVoltage() * 1000.0f + 0.5f));
          logEvent("POWER_L3", _d); }
      }
      /* 不 return：继续循环，每 5s 检查电池是否恢复（充电唤醒）
       * 后续 L3 守卫会跳过所有非必须操作 */
    } else if (powerManager.getPowerLevel() == POWER_L3_SHUTDOWN) {
      /* 电池恢复 → 退出 L3 关机，回到 L1 待机 */
      Serial.printf("[PWR] battery=%d%% -> recover from L3 SHUTDOWN\n", bat);
      powerManager.setPowerLevel(POWER_L1_STANDBY);
      uiSetScreenOn(true);
      { char _d[48]; snprintf(_d,sizeof(_d),"{\"pct\":%u,\"mv\":%u}",bat,
        (uint16_t)(powerManager.getBatteryVoltage() * 1000.0f + 0.5f));
        logEvent("POWER_L3_EXIT", _d); }
    }

    // L2: 反扣 ≥ 1min → 深度休眠（补传期间禁止 L2）
    {
    static unsigned long flipDownSince = 0;
    PoseFace rawFace = poseDetector.getRawFace();
    // millis() 49.7天回卷保护：now < flipDownSince 表示发生了回卷，重置计时器
    if (flipDownSince > now) flipDownSince = 0;
    if (rawFace == POSE_FLAT_DOWN) {
      if (flipDownSince == 0) flipDownSince = now;
      // 补传期间不进入 L2（WiFi 需要保持连接）
      if (now - flipDownSince >= POWER_L2_FLIP_MS && !keys.isLocked()
          && !network.isFlushActive()) {
        if (powerManager.getPowerLevel() != POWER_L2_DEEP_SLEEP) {
          // 反扣进休眠前结算当前专注工时（方案 Y：直接结算避免计时在休眠中继续累加）
          timerManager.enterStandby();
          powerManager.setPowerLevel(POWER_L2_DEEP_SLEEP);
          // L2 休眠降 IMU 采样率到 3Hz 省电（唤醒后再恢复 1000Hz）
          poseDetector.setAccelOdrLowPower();
          logEvent("POWER_L2", "{\"trigger\":\"flip\"}");
        }
      }
      // 平面朝下补传触发（路径 A）
      if (network.isWifiConnected() && network.hasPendingLogs()
          && !network.isFlushActive()) {
        network.flushBegin();
      }
    } else {
      flipDownSince = 0;
      // 设备翻起时暂停补传
      if (network.isFlushActive()) {
        network.flushPause();
      }
    }

    // L1 ↔ L0 切换
    if (powerManager.getPowerLevel() >= POWER_L2_DEEP_SLEEP) {
      // L2 以上不参与 L0/L1 评估
  } else if (!uiIsScreenOn() && now - lastScreenOnMs >= POWER_L1_IDLE_MS) {
      if (powerManager.getPowerLevel() != POWER_L1_STANDBY) {
        powerManager.setPowerLevel(POWER_L1_STANDBY);
        logEvent("POWER_L1", "{\"reason\":\"idle_timeout\"}");
      }
    } else {
      if (powerManager.getPowerLevel() != POWER_L0_NORMAL) {
        powerManager.setPowerLevel(POWER_L0_NORMAL);
        // 从 L2 唤醒时恢复 IMU 采样率到 1000Hz
        poseDetector.setAccelOdrNormal();
        logEvent("POWER_L0", "{\"reason\":\"wake\"}");
      }
    }
    }  // end L2 scope block
  }
  debugWatchdog_poke("power");

  /* L3 安全关机守卫：跳过所有非必须操作，只允许电池监测（充电唤醒） */
  if (powerManager.getPowerLevel() == POWER_L3_SHUTDOWN) return;

  // 4) 触屏
  checkTouch();
  debugWatchdog_poke("touch");

  // 5) UI 刷新
  uiUpdate();
  debugWatchdog_poke("ui");

  // 5a) 延迟屏幕切换（FLAT_DOWN 触发，先切界面再熄屏）
  if (s_pendingScreenSwitch) {
    s_pendingScreenSwitch = false;
    uiInvalidate();
    uiSetScreenOn(false);
  }

  // 5b) SD 卡延迟写入（在 LVGL flush 之后，下次 flush 之前）
  storage.flushPendingEvents();

#ifdef USE_LVGL
  /* 用判定时刻的实时时间，避免与循环开头捕获的 now 错位。
   * 弹窗标记 totalPopupShownMs / lowBatShownMs 在循环中途（按键/事件处理）才赋值，
   * 若用循环头的旧 now 比较，now < 标记值会导致 unsigned 相减下溢为极大值，
   * 使 >= 阈值 恒成立 → 弹窗被立即清空（实际持续时长≈0）。见 docs/lessons_learned/。 */
  unsigned long autoNow = millis();
  /* 低电量弹窗 2s 自动消失 */
  if (lowBatShownMs > 0 && autoNow - lowBatShownMs >= SCREEN_LOWBAT_POPUP_MS) {
    lowBatShownMs = 0;
    uiInvalidate(); /* 回到当前状态的屏幕 */
  }
  /* S9 总时长弹窗 3s 自动消失 */
  if (totalPopupShownMs > 0 && autoNow - totalPopupShownMs >= SCREEN_TOTAL_POPUP_MS) {
    totalPopupShownMs = 0;
    uiInvalidate(); /* 回到当前状态的屏幕 */
  }
#endif

  debugWatchdog_poke("display");

  network.tick();
  debugWatchdog_poke("network");
}

/**
 * @file debug_console.cpp
 * ChronoCube 调试控制台
 *
 * v5.3: 重构为 LVGL 无关架构
 *   - 通用命令（key/pose/state/lock/mute/timer/help/info）始终可用
 *   - LVGL 特定命令（screenshot/screen/rot）仅在 USE_LVGL 时编译
 *
 * 【维护约定】本模块的使用/命令/回传格式/截屏机制的权威说明见
 *   docs/serial_debug_console.md
 * 每次新增或修改命令、改动回传格式或截屏行为时，必须同步更新该文档。
 */
#include "config.h"
#include "debug_console.h"
#include "timer.h"
#include "keys.h"
#include "pose.h"
#include "pmu.h"
#include "audio.h"
#include "storage.h"
#include "network.h"
#include "i2c_bsp.h"
#include "font_loader.h"
#include "display.h"
#include "src/config_loader.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <WiFi.h>

/* ESP-IDF diagnostics */
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef USE_LVGL
#include "display.h"
#include "lvgl_bridge.h"
#include "lv_port_disp.h"
#include "touch.h"
#include "spi_bus_lock.h"
#include <esp_lcd_panel_ops.h>
/* #include "ui.h" — 已移除：LVGL 模式下屏幕开关通过 lvgl_bridge.h/lv_port_disp.h，非旧 UIManager */
#include "screenshot.h"
#endif

/* ==================== 常量 ==================== */
#define CMD_BUF_SIZE 64
#define CMD_ARGS_MAX 6

/* ==================== 帮助文本 ==================== */
static const char * const s_help_common =
  "=== ChronoCube Debug Console v3.0 ===\n"
  "COMMON COMMANDS:\n"
  "  help                        Show this help\n"
  "  info                        Dump system state\n"
  "  event [n]                   Show last n event log lines (default 5)\n"
  "  key <type>                  Simulate key press:\n"
  "      boot_click|boot_long|user_click|user_long\n"
  "  pose <face>                 Simulate pose change:\n"
  "      flat_up|flat_down|upright|left|right\n"
  "  state <name>                Force state machine:\n"
  "      standby|deep_focus|light_work|study|pause\n"
  "      deep_rest|light_rest|study_rest|emotion\n"
  "  lock 0|1                    Set lock state\n"
  "  mute                        Toggle mute\n"
  "  setvolume <hex>             Set ES8311 DAC volume (e.g. C0 = -0.5dB)\n"
  "  beep [freq|event|list]     Play tone: 'beep 2000 200', 'beep focus_start', or 'beep list'\n"
  "      Events: focus_start, pause, resume, cycle_end, rest_start,\n"
  "              rest_end, emotion_timeout, low_battery, lock, unlock\n"
  "  timer <sec>                 Set fake timer value\n"
  "  time [YYYY-MM-DD HH:MM:SS] Set RTC time (no arg = show current)\n"
  "  wifi                        WiFi connection status\n"
  "  sd                          SD card info\n"
  "  net                         Network status (WiFi + MQTT)\n"
  "  flush [begin|pause]         Offline log upload status/control\n"
  "  rtc                         RTC time and availability\n"
  "\n"
  "DIAGNOSTIC COMMANDS:\n"
  "  heap                        Detailed heap breakdown (DRAM/DMA/IRAM)\n"
  "  crash                       Last reset reason (POWERON/WDT/PANIC/BROWNOUT)\n"
  "  i2c                         Scan I2C bus for responding devices\n"
  "  i2c_dump <addr> <reg> [n]  Dump n bytes of I2C device registers\n"
  "  task                        FreeRTOS task list + stack high water marks\n"
  "  uptime                      Show millis() + RTC time since boot\n"
  "  stress <type> [n]           Stress test:\n"
  "      state <n>               Cycle n random states (default 20)\n"
  "      beep <n>                Play n rapid beeps (default 10)\n"
  "      all <n>                 Combined state+beep cycles\n"
  "  watchdog                    Module activity monitor (who's hung?)\n"
  "  loop                        Loop timing stats (min/max/avg ms)\n"
  "  fontcache                   Font cache hit/miss statistics\n"
  "  audiodebug [0|1]            Audio debug verbose: per-chunk I2S detail + zero-data detect\n"
  "  audiostat                   Audio health snapshot (ES8311 regs, power level, mute, last note)\n"
  "  mclk                        Probe GPIO19 MCLK physical signal (toggling test)\n";

#ifdef USE_LVGL
static const char * const s_help_lvgl =
  "LVGL COMMANDS:\n"
  "  screenshot                  Capture screen as raw RGB565 (serial)\n"
  "  screenshot_ppm              Capture screen as PPM (serial)\n"
  "  deadpix <color>             Full-screen solid color test (dead pixel check)\n"
  "  screenshot_sd               Save screen as 24-bit BMP to SD card\n"
  "                               → ChronoCube/screenshots/scr_####.bmp\n"
  "  screen <name>               Force screen:\n"
  "      standby|focus|pause|rest|emotion|summary|locked|lowbat|restend\n"
  "  rot <0-3>                   Test MADCTL rotation value\n"
  "  madctl <hex>                Send raw MADCTL value to LCD\n"
  "  setbright <0-100>           Set LCD brightness\n"
  "  volume <0-100>              Set audio volume (0=mute, 100=max)\n"
  "  powersave <0|1>             Enable/disable power-saving (screen-off) mode\n";
#endif

/* ==================== 辅助函数 ==================== */

static void printPrompt(void) { Serial.print("[DBG] "); }

static int tokMatch(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static int tokenize(char *line, const char **argv, int maxArgs) {
    int argc = 0;
    char *p = line;
    while (*p && argc < maxArgs) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* ==================== 通用命令 ==================== */

static void onCmdHelp(void) {
    Serial.print(s_help_common);
#ifdef USE_LVGL
    Serial.print(s_help_lvgl);
#endif
}

static void onCmdInfo(void) {
    SystemState st = timerManager.getState();
    unsigned long elapsed = timerManager.getCurrentDuration();
    unsigned long total = timerManager.getCycleTotal();
    unsigned long today = timerManager.getTotalToday();
    uint8_t bat = powerManager.getBatteryPercent();

    Serial.printf("State: %s\n", timerManager.stateNameEn());
    Serial.printf("Timer: %lu / %lus (%d%%)\n", elapsed, total,
        (total > 0) ? (int)(elapsed * 100 / total) : 0);
    Serial.printf("Today: %lus total\n", today);
    Serial.printf("Battery: %d%%\n", bat);
    Serial.printf("Locked: %s\n", keys.isLocked() ? "yes" : "no");
    Serial.printf("Muted: %s\n", keys.isMuted() ? "yes" : "no");
    Serial.printf("Power: L%d  powersave=%s\n", powerManager.getPowerLevel(),
        configGetRuntime().powerSaveEnabled ? "on" : "off");

    // Timer details
    unsigned long eff  = timerManager.getEffectiveToday();
    unsigned long ineff = timerManager.getIneffectiveToday();
    Serial.printf("Timer: eff=%lus ineff=%lus (%.0f%% effective)\n",
        eff, ineff, today > 0 ? (eff * 100.0f / today) : 0);

    // Per-mode today breakdown
    unsigned long df2  = timerManager.getModeToday(STATE_DEEP_FOCUS);
    unsigned long lw2  = timerManager.getModeToday(STATE_LIGHT_WORK);
    unsigned long st2  = timerManager.getModeToday(STATE_STUDY);
    Serial.printf("Today modes: deep=%lus light=%lus study=%lus\n", df2, lw2, st2);

    // Pause info
    if (timerManager.getHadPause()) {
        Serial.printf("Pause: yes  pause_dur=%lus  saved_work=%lus\n",
            (unsigned long)timerManager.getPauseDuration(),
            (unsigned long)timerManager.getSavedWorkDuration());
    }

    // Network status
    if (network.isWifiConnected()) {
        char ipStr[16] = "";
        network.getWifiIp(ipStr, sizeof(ipStr));
        Serial.printf("WiFi: connected  RSSI=%d dBm  IP=%s\n",
            network.getWifiRssi(), ipStr);
        Serial.printf("MQTT: %s\n", network.isMqttConnected() ? "connected" : "disconnected");
    } else {
        Serial.println("WiFi: disconnected");
    }
#ifdef USE_LVGL
    Serial.printf("Screen: %s\n", lvglIsScreenEnabled() ? "on" : "off");
    Serial.printf("Rotation: %d°\n", display.getRotation());
#endif
    PoseFace pf = poseDetector.getFace();
    static const char *pn[] = {"FLAT_UP", "FLAT_DOWN", "UPRIGHT",
        "LEFT", "RIGHT", "INVERTED", "UNKNOWN"};
    Serial.printf("Pose: %s (stable=%d)\n", pn[pf], poseDetector.isStable());
    int freeHeap = 0, minHeap = 0;
#ifdef USE_LVGL
    freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    minHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
#endif
    Serial.printf("Heap: free=%d min=%d bytes\n", freeHeap, minHeap);
}

static void onCmdKey(const char *type) {
    if (tokMatch(type, "boot_click")) {
        debug_inject_key = KEY_EVT_BOOT_CLICK;
    } else if (tokMatch(type, "boot_long")) {
        debug_inject_key = KEY_EVT_BOOT_LONG;
    } else if (tokMatch(type, "user_click")) {
        debug_inject_key = KEY_EVT_USER_CLICK;
    } else if (tokMatch(type, "user_long")) {
        debug_inject_key = KEY_EVT_USER_LONG;
    } else {
        Serial.printf("Unknown key type: %s\n", type);
        return;
    }
    Serial.printf("[DBG] Key '%s' injected\n", type);
}

// [DEBUG ONLY] 串口姿态注入：绕过防抖/状态机直接喂 pose，仅供物理 USB 连接调试（无鉴权，需物理接触设备）
static void onCmdPose(const char *face) {
    PoseFace f = POSE_UNKNOWN;
    if (tokMatch(face, "flat_up"))     f = POSE_FLAT_UP;
    else if (tokMatch(face, "flat_down")) f = POSE_FLAT_DOWN;
    else if (tokMatch(face, "upright"))   f = POSE_UPRIGHT;
    else if (tokMatch(face, "left"))      f = POSE_LEFT;
    else if (tokMatch(face, "right"))     f = POSE_RIGHT;
    else {
        Serial.printf("Unknown pose: %s\n", face);
        return;
    }
    debug_inject_pose = f;
    debug_inject_pose_stable = true;
    Serial.printf("[DBG] Pose '%s' injected\n", face);
}

// [DEBUG ONLY] 串口状态机注入：直接调用 enterFocus/enterRest 绕过正常转移，仅供物理 USB 调试（无鉴权，需物理接触设备）
static void onCmdState(const char *name) {
#ifdef USE_LVGL
    if (!lvglIsScreenEnabled()) { lvglSetScreenEnabled(true); }
#endif

    SystemState st;
    if (tokMatch(name, "standby"))        st = STATE_STANDBY;
    else if (tokMatch(name, "deep_focus"))  st = STATE_DEEP_FOCUS;
    else if (tokMatch(name, "light_work"))  st = STATE_LIGHT_WORK;
    else if (tokMatch(name, "study"))       st = STATE_STUDY;
    else if (tokMatch(name, "pause"))       st = STATE_PAUSE;
    else if (tokMatch(name, "deep_rest"))   st = STATE_DEEP_REST;
    else if (tokMatch(name, "light_rest"))  st = STATE_LIGHT_REST;
    else if (tokMatch(name, "study_rest"))  st = STATE_STUDY_REST;
    else if (tokMatch(name, "emotion"))     st = STATE_EMOTION_PICK;
    else {
        Serial.printf("Unknown state: %s\n", name);
        return;
    }

    if (st == STATE_STANDBY) {
        timerManager.enterStandby();
    } else if (st == STATE_EMOTION_PICK) {
        timerManager.enterEmotionPick();
    } else if (st == STATE_PAUSE) {
        timerManager.enterPause();
    } else if (st == STATE_DEEP_REST || st == STATE_LIGHT_REST || st == STATE_STUDY_REST) {
        // Prefer current work mode if we're in one, otherwise derive from rest type
        SystemState cur = timerManager.getState();
        SystemState prevWork = STATE_DEEP_FOCUS;
        if      (cur == STATE_DEEP_FOCUS) prevWork = STATE_DEEP_FOCUS;
        else if (cur == STATE_LIGHT_WORK) prevWork = STATE_LIGHT_WORK;
        else if (cur == STATE_STUDY)      prevWork = STATE_STUDY;
        else if (st == STATE_LIGHT_REST)  prevWork = STATE_LIGHT_WORK;
        else if (st == STATE_STUDY_REST)  prevWork = STATE_STUDY;
        timerManager.enterRest(prevWork);
    } else {
        timerManager.enterFocus(st, true);
    }

#ifdef USE_LVGL
    lvglBridge_showScreenForState(st);
#endif
    Serial.printf("[DBG] State forced to '%s'\n", name);
}

static void onCmdLock(const char *arg) {
    bool lock = tokMatch(arg, "1") || tokMatch(arg, "true") || tokMatch(arg, "on");
    keys.setLocked(lock);
#ifdef USE_LVGL
    lvglBridge_setLocked(lock);
    if (!lock) {
        /* 解锁时恢复到当前状态对应的屏幕 */
        if (!lvglIsScreenEnabled()) { lvglSetScreenEnabled(true); }
        lvglBridge_showScreenForState(timerManager.getState());
    }
#endif
    Serial.printf("[DBG] Lock %s\n", lock ? "ON" : "OFF");
}

static void onCmdMute(void) {
    keys.toggleMute();
    bool muted = keys.isMuted();
    audio.setMuted(muted);
    Serial.printf("[DBG] Mute %s\n", muted ? "ON" : "OFF");
}

static void onCmdBeep(const char *freqArg, const char *durArg) {
    /* 支持三种模式：
     *   beep <freq> <dur_ms>  — 自定义频率/时长
     *   beep <event_name>     — 播放事件音效（如 focus_start, pause 等）
     *   beep list             — 列出所有可用音效名称
     */
    if (!freqArg) {
        Serial.println("[DBG] Usage: beep <freq_hz> <dur_ms>  or  beep <event_name>");
        Serial.println("      Events: focus_start, pause, resume, cycle_end,");
        Serial.println("              rest_start, rest_end, emotion_timeout,");
        Serial.println("              low_battery, lock, unlock");
        Serial.println("      beep list  — show all event names");
        return;
    }

    /* 列出所有音效 */
    if (tokMatch(freqArg, "list")) {
        Serial.println("[DBG] Available sounds:");
        Serial.println("  focus_start     — C5→E5→G5 上行琶音 (专注开始)");
        Serial.println("  pause           — E5→C5 下行 (暂停)");
        Serial.println("  resume          — C5→E5 上行 (恢复专注)");
        Serial.println("  cycle_end       — C5-E5-G5→C6 胜利上行 (周期完成)");
        Serial.println("  rest_start      — G5→E5→C5 下行 (休息开始)");
        Serial.println("  rest_end        — C5→E5 缓慢上行 (休息结束)");
        Serial.println("  emotion_timeout — E5→G5 轻柔双音 (情绪超时)");
        Serial.println("  low_battery     — A4→E4→A4 低沉三连 (低电量)");
        Serial.println("  lock            — C5 单音干脆 (锁定)");
        Serial.println("  unlock          — C5→E5 快速上行 (解锁)");
        return;
    }

    /* 事件名模式：临时解静音播放 */
    bool wasMutedEvent = audio.isMuted();
    if (wasMutedEvent) audio.setMuted(false);
    if (audio.playSoundByName(freqArg)) {
        Serial.printf("[DBG] Beep event: %s\n", freqArg);
        if (wasMutedEvent) audio.setMuted(true);
        return;
    }
    if (wasMutedEvent) audio.setMuted(true);

    /* 频率模式 */
    float freq = atof(freqArg);
    float dur  = durArg ? atof(durArg) : 200.0f;
    // C2-fix: NaN 校验（atof 对非法输入返回 0.0，但对部分异常输入可能返回 NaN）
    if (freq != freq || dur != dur) { Serial.println("[DBG] Invalid number (NaN)"); return; }
    if (freq <= 0 || freq > 20000)  { Serial.println("[DBG] Freq out of range (1-20000 Hz)"); return; }
    if (dur <= 0 || dur > 5000)     { Serial.println("[DBG] Duration out of range (1-5000 ms)"); return; }
    bool wasMuted = audio.isMuted();
    audio.setMuted(false);
    audio.beep(freq, dur);
    audio.setMuted(wasMuted);
    Serial.printf("[DBG] Beep: %.0fHz %.0fms\n", freq, dur);
}

/* MCLK 物理信号自检：临时开启 GPIO(MCLK) 输入缓冲，统计翻转次数。
 * 若大量翻转 → MCLK 信号存在（问题在下游模拟/功放）；
 * 若几乎为 0  → MCLK 没输出（ESP32-C6 I2S 未驱动该脚，DAC 无内部时钟 → 无声）。
 * 测试后恢复为输出方向（I2S 仍驱动 MCLK），无需重启。 */
static void onCmdMclk() {
  const gpio_num_t pin = (gpio_num_t)PIN_I2S_MCLK;  // GPIO19
  Serial.printf("[MCLK] probe GPIO%d (MCLK) for toggling (I2S output kept active)...\n", (int)pin);
  gpio_input_enable(pin);  // 仅开启输入缓冲，不切换方向，保留 I2S MCLK 驱动
  int last = gpio_get_level(pin);
  int trans = 0;
  volatile uint32_t n = 300000;
  for (uint32_t i = 0; i < n; i++) {
    int v = gpio_get_level(pin);
    if (v != last) { trans++; last = v; }
  }
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);  // 恢复为输出（I2S 驱动 MCLK），同时关闭输入缓冲
  Serial.printf("[MCLK] GPIO%d transitions in %u samples = %d\n", (int)pin, (unsigned)n, trans);
  Serial.printf("[MCLK] => %s\n",
                trans > 20 ? "MCLK SIGNAL PRESENT (toggling — audio path downstream)" :
                             "MCLK STATIC/ABSENT (no signal — root cause of silence)");
  Serial.println("[MCLK] I2S MCLK output preserved; no reboot needed");
}

static void onCmdTime(const char *dateArg, const char *timeArg) {
  if (!dateArg) {
    /* 显示当前时间 */
    time_t now = time(nullptr);
    struct tm *tm = localtime(&now);
    if (tm && now > 1000000000) {
      Serial.printf("[TIME] %04d-%02d-%02d %02d:%02d:%02d\n",
                    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                    tm->tm_hour, tm->tm_min, tm->tm_sec);
    } else {
      Serial.println("[TIME] No valid time (RTC not set)");
    }
    return;
  }
  /* 解析 time YYYY-MM-DD HH:MM:SS 或 time YYYY-MM-DD（时间默认 12:00:00） */
  int y = 0, mo = 0, d = 0, h = 12, mi = 0, s = 0;
  if (sscanf(dateArg, "%d-%d-%d", &y, &mo, &d) != 3) {
    Serial.println("[TIME] Usage: time YYYY-MM-DD [HH:MM:SS]");
    return;
  }
  if (timeArg) sscanf(timeArg, "%d:%d:%d", &h, &mi, &s);

  if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
      h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
    Serial.println("[TIME] Invalid date/time values");
    return;
  }

  /* 写入 RTC */
  storage.setRtcDateTime((uint16_t)y, (uint8_t)mo, (uint8_t)d,
                         (uint8_t)h, (uint8_t)mi, (uint8_t)s);

  /* 同步系统时间 */
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon  = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min  = mi;
  t.tm_sec  = s;
  time_t epoch = mktime(&t);
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, NULL);

  Serial.printf("[TIME] Set to %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
}

static void onCmdTimer(const char *arg) {
  // timer            → 显示当前计时状态 + 今日汇总
  // timer <sec>      → 设置已流逝秒数（调试用，0=重置）
  if (arg) {
    unsigned long sec = (unsigned long)atoi(arg);
    timerManager.debugSetElapsed(sec);
    Serial.printf("[DBG] Timer: elapsed set to %lus\n", sec);
  }
  // 当前状态
  unsigned long dur = timerManager.getCurrentDuration();
  unsigned long total = timerManager.getCycleTotal();
  Serial.printf("[DBG] Timer: state=%s  elapsed=%lus/%lus (%.0f%%)\n",
                timerManager.stateName(), dur, total,
                total > 0 ? (dur * 100.0f / total) : 0);
  // 今日汇总
  unsigned long today = timerManager.getTotalToday();
  unsigned long eff  = timerManager.getEffectiveToday();
  unsigned long ineff = timerManager.getIneffectiveToday();
  Serial.printf("[DBG] Today: total=%lus  eff=%lus  ineff=%lus  (%.0f%% eff)\n",
                today, eff, ineff,
                today > 0 ? (eff * 100.0f / today) : 0);
  // Per-mode
  Serial.printf("[DBG] Modes: deep=%lus  light=%lus  study=%lus\n",
                (unsigned long)timerManager.getModeToday(STATE_DEEP_FOCUS),
                (unsigned long)timerManager.getModeToday(STATE_LIGHT_WORK),
                (unsigned long)timerManager.getModeToday(STATE_STUDY));
  // 暂停信息
  if (timerManager.getHadPause()) {
    Serial.printf("[DBG] Pause: yes  dur=%lus  saved=%lus\n",
                  (unsigned long)timerManager.getPauseDuration(),
                  (unsigned long)timerManager.getSavedWorkDuration());
  }
  // 上次日重置
  uint16_t ry; uint8_t rm, rd;
  timerManager.getLastResetDate(ry, rm, rd);
  Serial.printf("[DBG] Last daily reset: %d/%02d/%02d\n", ry, rm, rd);
}

/* ==================== 网络/存储/RTC 命令 ==================== */

/* --- event: 查看事件日志 --- */
static void onCmdEvent(const char *arg) {
    int n = arg ? atoi(arg) : 5;
    if (n < 1) n = 1;
    if (n > 20) n = 20;

    // 构建今天的事件日志路径
    char path[64] = "";
    if (storage.isRtcAvailable()) {
        RtcDateTime dt;
        if (storage.getRtcDateTime(dt)) {
            snprintf(path, sizeof(path), "/sdcard/ChronoCube/logs/%02d%02d%02d.jsonl",
                (uint8_t)(dt.year % 100), dt.month, dt.day);
        }
    }
    if (path[0] == '\0') {
        Serial.println("[EVENT] RTC not available, cannot locate log file");
        return;
    }

    Serial.printf("[EVENT] Reading last %d from %s\n", n, path);

    // C1-fix: 先获取文件大小，从尾部倒推，减少 SPI2 锁持有时间
    if (!spi2_lock("event")) { Serial.println("[EVENT] SPI2 lock failed"); return; }
    FILE *f = fopen(path, "r");
    if (!f) {
        spi2_unlock("event");
        Serial.printf("[EVENT] No events today (%s not found)\n", path);
        return;
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) {
        fclose(f);
        spi2_unlock("event");
        Serial.println("  (no events)");
        return;
    }

    // 从末尾向前读取一个合理大小的块（最大 8KB，约 16-30 行）
    // 避免扫描整个文件，减少锁持有时间
    #define EVENT_READ_CHUNK  8192
    long readStart = (fsize > EVENT_READ_CHUNK) ? (fsize - EVENT_READ_CHUNK) : 0;
    fseek(f, readStart, SEEK_SET);

    // 在块内统计行数
    int totalInChunk = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') totalInChunk++;
    }

    // 如果块内行数 < n 且文件开头 > 0，说明需要读更多（极端长行情况）
    // 正常日志每行 ~80-200 字节，8KB 足够装 40+ 行，n<=20 够用
    rewind(f);
    fseek(f, readStart, SEEK_SET);

    // 跳过多余的行
    int skip = (totalInChunk > n) ? (totalInChunk - n) : 0;
    int sc = 0;
    if (readStart > 0) {
        // 如果不是从文件开头读，跳过第一行（可能不完整）
        while ((ch = fgetc(f)) != EOF && ch != '\n') {}
        if (ch == '\n') sc = -1; // 第一行不完整，少算一行
    }
    while (sc < skip && (ch = fgetc(f)) != EOF) {
        if (ch == '\n') sc++;
    }

    // 逐行读取并直接打印（不再预缓冲到 outBuf[20*512] 的 10KB 栈数组，
    // 否则会撑爆 loopTask 栈导致 Guru Meditation 崩机，见 2026-07-29 串口调试）
    // 说明：原设计“先读进内存、释放 SPI2 锁后再打印”是为避免串口输出期间阻塞 LCD 刷新；
    // 但 event 是手动调试命令，边读边打印（持锁期间）最多引入 ~170ms 串口停顿，远优于崩溃。
    char lineBuf[512];
    int printed = 0;
    while (printed < n && fgets(lineBuf, sizeof(lineBuf), f)) {
        size_t len = strlen(lineBuf);
        if (len > 0 && lineBuf[len - 1] == '\n') lineBuf[len - 1] = '\0';
        Serial.printf("  %s\n", lineBuf);
        printed++;
    }
    fclose(f);
    spi2_unlock("event");

    if (printed == 0) {
        Serial.println("  (no events)");
    }
}

static void onCmdWifi(void) {
  Serial.println("=== WIFI STATUS ===");
  int status = WiFi.status();
  const char *statusStr;
  switch (status) {
    case WL_CONNECTED:      statusStr = "CONNECTED"; break;
    case WL_DISCONNECTED:   statusStr = "DISCONNECTED"; break;
    case WL_IDLE_STATUS:    statusStr = "IDLE"; break;
    case WL_NO_SSID_AVAIL:  statusStr = "NO_SSID_AVAIL"; break;
    case WL_CONNECTION_LOST: statusStr = "CONNECTION_LOST"; break;
    default:                statusStr = "UNKNOWN"; break;
  }
  Serial.printf("  Status : %s (%d)\n", statusStr, status);
  Serial.printf("  SSID   : %s\n", WIFI_SSID);
  if (status == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    char ipBuf[16];
    snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macBuf[18];
    snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("  IP     : %s\n", ipBuf);
    Serial.printf("  RSSI   : %d dBm\n", WiFi.RSSI());
    Serial.printf("  MAC    : %s\n", macBuf);
  }
}

static void onCmdSd(void) {
  storage.printSDInfo();
}

static void onCmdNet(void) {
  Serial.println("=== NETWORK STATUS ===");
  int wifiStatus = WiFi.status();
  bool wifiOk = (wifiStatus == WL_CONNECTED);
  Serial.printf("  WiFi   : %s", wifiOk ? "connected" : "disconnected");
  if (wifiOk) {
    IPAddress ip = WiFi.localIP();
    char ipBuf[16];
    snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    Serial.printf(" (%s)", ipBuf);
  }
  Serial.println();
  Serial.printf("  MQTT   : %s\n", network.isConnected() ? "connected" : "disconnected");
  Serial.printf("  SD     : %s\n", storage.isReady() ? "ready" : "not ready");
}

/* ==================== Flush 调测命令 ==================== */

static void onCmdFlush(const char *sub, int argc, const char **argv) {
  if (sub && tokMatch(sub, "begin")) {
    if (network.isFlushActive()) {
      Serial.println("[DBG] Flush already active. Use 'flush pause' first.");
    } else if (!network.hasPendingLogs()) {
      Serial.println("[DBG] No pending logs to flush.");
    } else {
      network.flushBegin();
      Serial.println("[DBG] Flush started manually.");
    }
    return;
  }

  if (sub && tokMatch(sub, "pause")) {
    if (!network.isFlushActive()) {
      Serial.println("[DBG] Flush not active.");
    } else {
      network.flushPause();
      Serial.println("[DBG] Flush paused.");
    }
    return;
  }

  // ── 默认: flush status ──
  Serial.println("=== FLUSH STATUS ===");
  Serial.printf("  Active       : %s\n", network.isFlushActive() ? "YES" : "no");
  if (network.isFlushActive()) {
    Serial.printf("  Waiting ACK  : %s\n", network.isFlushWaitingAck() ? "YES" : "no");
    Serial.printf("  Next seq     : %u\n", network.getFlushNextSeq());
    Serial.printf("  Files        : %d/%d (current idx: %d)\n",
                  network.getFlushFileIdx() + 1,
                  network.getFlushFileCount(),
                  network.getFlushFileIdx());
    Serial.printf("  Line skip    : %d\n", network.getFlushLineSkip());
    Serial.printf("  Batch max    : %d/tick\n", network.getFlushBatchMax());
    Serial.printf("  Ack timeout  : %d ms\n", network.getFlushAckTimeoutMs());
    if (network.isFlushWaitingAck()) {
      unsigned long elapsed = millis() - network.getFlushAckSentMs();
      int remain = network.getFlushAckTimeoutMs() - (int)elapsed;
      Serial.printf("  Ack sent     : %lu ms ago (remain: %d ms)\n",
                    elapsed, (remain > 0) ? remain : 0);
      if (remain <= 0) {
        Serial.println("  ⚠ ACK TIMEOUT — will retry next cycle");
      }
    }
  } else {
    Serial.printf("  Has pending  : %s\n", network.hasPendingLogs() ? "YES" : "no");
  }
  Serial.println("  Commands: flush begin | flush pause | flush status");
}

static void onCmdRtc(void) {
  Serial.println("=== RTC STATUS ===");
  Serial.printf("  Available: %s\n", storage.isRtcAvailable() ? "yes" : "no");
  RtcDateTime dt;
  if (storage.getRtcDateTime(dt)) {
    Serial.printf("  Time     : %04d-%02d-%02d %02d:%02d:%02d\n",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  } else {
    Serial.println("  Time     : invalid or not set");
  }
}

/* ==================== 诊断命令 ==================== */

static void onCmdHeap(void) {
    size_t dramFree  = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t dramMin   = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t dramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t dramTotal = heap_caps_get_total_size(MALLOC_CAP_8BIT);

    size_t dmaFree   = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dmaMin    = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
    size_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t dmaTotal  = heap_caps_get_total_size(MALLOC_CAP_DMA);

    size_t iramFree  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t iramTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

    Serial.println("=== HEAP DIAGNOSTICS ===");
    Serial.printf("DRAM (8-bit): %u / %u free (%d%%), min=%u, largest=%u\n",
        dramFree, dramTotal, dramTotal ? (int)(dramFree * 100 / dramTotal) : 0,
        dramMin, dramLargest);
    Serial.printf("DMA        : %u / %u free (%d%%), min=%u, largest=%u\n",
        dmaFree, dmaTotal, dmaTotal ? (int)(dmaFree * 100 / dmaTotal) : 0,
        dmaMin, dmaLargest);
    Serial.printf("IRAM       : %u / %u free (%d%%)\n",
        iramFree, iramTotal, iramTotal ? (int)(iramFree * 100 / iramTotal) : 0);

    // 警告阈值
    if (dramFree < 32768)  Serial.println("[HEAP] WARNING: DRAM critically low (<32KB)!");
    if (dmaMin < 16384)    Serial.println("[HEAP] WARNING: DMA min free < 16KB — screen/lvgl risk!");
}

static void onCmdCrash(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    static const char *names[] = {
        "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT",
        "TASK_WDT", "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO", "JTAG"
    };
    const char *name = (reason < 12) ? names[reason] : "?";
    Serial.printf("Last reset: %s (%d)\n", name, (int)reason);

    // 额外上下文
    switch (reason) {
        case ESP_RST_PANIC:
            Serial.println("  → Guru Meditation / LoadAccessFault / etc.");
            Serial.println("  → Check: buf overflow, null deref, stack smash");
            break;
        case ESP_RST_INT_WDT:
            Serial.println("  → Interrupt watchdog — ISR took too long or deadlock");
            break;
        case ESP_RST_TASK_WDT:
            Serial.println("  → Task watchdog — loop() or FreeRTOS task blocked too long");
            break;
        case ESP_RST_BROWNOUT:
            Serial.println("  → Brownout — voltage sagged below threshold");
            break;
        case ESP_RST_POWERON:
            Serial.println("  → Normal cold boot / flash");
            break;
        case ESP_RST_SW:
            Serial.println("  → Software reset (esp_restart / serial DTR)");
            break;
        default: break;
    }
}

static void onCmdI2c(void) {
    Serial.println("=== I2C BUS SCAN (addr 0x01-0x7F) ===");
    I2CBus::scan(Serial);
    Serial.println("Expected devices:");
    Serial.printf("  0x18/0x19 = ES8311  audio codec\n");
    Serial.printf("  0x34       = AXP2101  PMU\n");
    Serial.printf("  0x51       = PCF85063 RTC\n");
    Serial.printf("  0x5A       = CST9220  touch\n");
    Serial.printf("  0x6B       = QMI8658  IMU\n");
}

static void onCmdI2cDump(const char *addrArg, const char *startRegArg, const char *countArg) {
    if (!addrArg || !startRegArg) {
        Serial.println("Usage: i2c_dump <addr_hex> <start_reg_hex> [count_dec]");
        return;
    }
    uint8_t addr = (uint8_t)strtol(addrArg, NULL, 16);
    uint8_t reg  = (uint8_t)strtol(startRegArg, NULL, 16);
    int count = countArg ? atoi(countArg) : 16;
    if (count < 1 || count > 256) count = 16;

    if (!I2CBus::probe(addr)) {
        Serial.printf("[I2C] No device at 0x%02X\n", addr);
        return;
    }
    Serial.printf("[I2C] Dump 0x%02X regs 0x%02X-0x%02X:\n",
        addr, reg, reg + count - 1);
    for (int i = 0; i < count; i += 16) {
        Serial.printf("  %02X:", reg + i);
        for (int j = 0; j < 16 && (i + j) < count; j++) {
            uint8_t val = 0;
            I2CBus::readReg8(addr, reg + i + j, &val);
            Serial.printf(" %02X", val);
        }
        Serial.println();
    }
}

static void onCmdTask(void) {
    Serial.println("=== FREERTOS TASK LIST ===");
    // C3-fix: vTaskList 不接受缓冲区大小，有溢出风险。
    // 按任务数动态分配，且设置上限，防止极端情况栈溢出。
    UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    if (taskCount == 0) taskCount = 1;
    // 每个任务约 40 字节，额外留 256 字节表头/余量
    size_t bufSize = (size_t)taskCount * 48 + 256;
    if (bufSize > 4096) bufSize = 4096;  // 硬上限 4KB
    char *buf = (char *)malloc(bufSize);
    if (!buf) {
        Serial.println("[TASK] malloc failed");
        return;
    }
    buf[0] = '\0';
    vTaskList(buf);
    buf[bufSize - 1] = '\0';  // 保险：强制 null 终止
    Serial.print(buf);
    free(buf);

    // Show stack high water mark for this task (loop task)
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(self);
    Serial.printf("This task (loop) stack HWM: %u words free\n", (unsigned)hwm);
}

/* --- deadpix: 坏点测试 --- */
static void onCmdDeadPix(const char *colorArg) {
    if (!colorArg) {
        Serial.println("Usage: deadpix <color>");
        Serial.println("  black | white | red | green | blue | yellow | cyan | magenta | orange");
        return;
    }
    uint16_t color;
    if      (tokMatch(colorArg, "black"))   color = COLOR_BLACK;
    else if (tokMatch(colorArg, "white"))   color = COLOR_WHITE;
    else if (tokMatch(colorArg, "red"))     color = COLOR_RED;
    else if (tokMatch(colorArg, "green"))   color = COLOR_GREEN;
    else if (tokMatch(colorArg, "blue"))    color = COLOR_BLUE;
    else if (tokMatch(colorArg, "yellow"))  color = COLOR_YELLOW;
    else if (tokMatch(colorArg, "cyan"))    color = COLOR_CYAN;
    else if (tokMatch(colorArg, "magenta")) color = COLOR_MAGENTA;
    else if (tokMatch(colorArg, "orange"))  color = COLOR_ORANGE;
    else { color = (uint16_t)strtol(colorArg, NULL, 16); }
    Serial.printf("[DEADPIX] Full-screen pure color 0x%04X\n", color);
    display.deadPixelTest(color);
}

/* --- fontcache: 字库缓存命中率 --- */
static void onCmdFontCache(void) {
    uint32_t cnHits, cnMisses, enHits, enMisses;
    fontLoader.getCacheStats(cnHits, cnMisses, enHits, enMisses);
    uint32_t cnTotal = cnHits + cnMisses;
    uint32_t enTotal = enHits + enMisses;
    Serial.printf("CN cache: hits=%lu misses=%lu total=%lu (%.1f%% hit)\n",
        cnHits, cnMisses, cnTotal,
        cnTotal > 0 ? (cnHits * 100.0f / cnTotal) : 0.0f);
    Serial.printf("EN cache: hits=%lu misses=%lu total=%lu (%.1f%% hit)\n",
        enHits, enMisses, enTotal,
        enTotal > 0 ? (enHits * 100.0f / enTotal) : 0.0f);
}

static void onCmdUptime(void) {
    unsigned long ms = millis();
    unsigned long s = ms / 1000;
    unsigned long m = s / 60;
    unsigned long h = m / 60;
    Serial.printf("Uptime: %luh %lum %lus (%lums)\n",
        h, m % 60, s % 60, ms);

    // RTC time
    if (storage.isRtcAvailable()) {
        RtcDateTime dt;
        if (storage.getRtcDateTime(dt)) {
            Serial.printf("RTC:    %04d-%02d-%02d %02d:%02d:%02d\n",
                dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
        } else {
            Serial.println("RTC:    (read failed)");
        }
    } else {
        Serial.println("RTC:    not available");
    }
}

static void onCmdStress(const char *type, const char *argN) {
    int n = argN ? atoi(argN) : 0;
    if (n <= 0) n = (tokMatch(type, "state")) ? 20 : 10;
    if (n > 500) { Serial.println("[STRESS] Max 500 iterations"); n = 500; }

    if (tokMatch(type, "state")) {
        static const char *states[] = {
            "standby", "deep_focus", "light_work", "study",
            "pause", "deep_rest", "light_rest", "study_rest", "emotion"
        };
        int nStates = sizeof(states) / sizeof(states[0]);
        Serial.printf("[STRESS] Cycling %d random states...\n", n);
        unsigned long t0 = millis();
        for (int i = 0; i < n; i++) {
            onCmdState(states[i % nStates]);
            // 不给 lv_timer_handler 机会 — 纯测试 onCmdState 路径稳定性
        }
        unsigned long dt = millis() - t0;
        Serial.printf("[STRESS] %d states in %lums (%.1f/s)\n",
            n, dt, dt > 0 ? (float)n * 1000.0f / (float)dt : 0.0f);
    } else if (tokMatch(type, "beep")) {
        Serial.printf("[STRESS] Playing %d rapid beeps...\n", n);
        unsigned long t0 = millis();
        for (int i = 0; i < n; i++) {
            onCmdBeep("800", "30");  // 800Hz 30ms → short rapid ticks
        }
        unsigned long dt = millis() - t0;
        Serial.printf("[STRESS] %d beeps in %lums (%.1f/s)\n",
            n, dt, dt > 0 ? (float)n * 1000.0f / (float)dt : 0.0f);
    } else if (tokMatch(type, "all")) {
        unsigned long t0 = millis();
        Serial.printf("[STRESS] Full test: %d state+beep cycles...\n", n);
        for (int i = 0; i < n; i++) {
            onCmdBeep("800", "30");
            onCmdState("deep_focus");
            onCmdState("deep_rest");
            onCmdState("standby");
        }
        unsigned long dt = millis() - t0;
        Serial.printf("[STRESS] %d full cycles in %lums (%.1f/s)\n",
            n, dt, dt > 0 ? (float)n * 1000.0f / (float)dt : 0.0f);
    } else {
        Serial.printf("[STRESS] Unknown type: %s (try 'state' or 'beep')\n", type);
    }
}

/* ==================== LVGL 特定命令 ==================== */
#ifdef USE_LVGL

/* Strip buffer: 480×25×2 = 24000 bytes.
 * Static allocation (.bss) avoids heap fragmentation risk — no OOM fallback needed.
 * Trade-off: permanently reserves 24KB (~20% of 120KB DRAM) for a debug feature.
 * Acceptable because screenshot is a critical debugging tool. */
#define SCREENSHOT_STRIP_H  25
#define SCREENSHOT_STRIP_PX (LCD_H_RES * SCREENSHOT_STRIP_H)
static uint16_t s_scr_buf[SCREENSHOT_STRIP_PX];
static int s_stripYStart = 0;
static int s_stripYEnd   = 0;

/* LVGL v9 PARTIAL mode: one lv_timer_handler() call processes ALL dirty areas in
 * the current batch. A full-screen invalidation (480×480) with 480×50 draw buffers
 * takes ~10 tiles to render. The draw dispatch runs synchronously within each
 * timer_handler call. 5 iterations with 2ms delay gives margin for edge cases
 * (slow render, back-to-back batches) while being 8× faster than the old 40-iter loop. */
#define SCR_STRIP_ITER      5
/* Post-recovery re-render: enough iterations for LVGL to push a full 480-line frame */
#define POST_RECOVERY_ITER  24

/* Flush callback for screenshot: copies rendered pixels into s_scr_buf,
 * filtering by the current strip Y range [s_stripYStart, s_stripYEnd). */
static void scrFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)disp;
    /* s_scr_buf is static .bss array — always valid, no NULL check needed */
    int w = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; y++) {
        if (y >= s_stripYStart && y < s_stripYEnd) {
            int destY = y - s_stripYStart;
            memcpy(&s_scr_buf[destY * LCD_H_RES + area->x1],
                   &px_map[(y - area->y1) * w * 2], w * 2);
        }
    }
}

/* Mock fallback: fills strips with test pattern when OOM.
 * USB CDC ignores baud rate settings — no switching needed. */
static bool screenshotDoMock(void) {
    uint16_t *strip = (uint16_t *)heap_caps_malloc(LCD_H_RES * SCREENSHOT_STRIP_H * 2, MALLOC_CAP_DMA);
    if (!strip) { Serial.println("[SCR] ERR: OOM"); return false; }
    bool prev = lvglIsScreenEnabled();
    lvglSetScreenEnabled(false);
    lv_timer_handler();
    uint8_t hdr[6] = {0xCC, 0x53, LCD_H_RES & 0xFF, (LCD_H_RES >> 8) & 0xFF, LCD_V_RES & 0xFF, (LCD_V_RES >> 8) & 0xFF};
    Serial.write(hdr, 6);
    for (int y = 0; y < LCD_V_RES; y += SCREENSHOT_STRIP_H) {
        int h = (y + SCREENSHOT_STRIP_H > LCD_V_RES) ? LCD_V_RES - y : SCREENSHOT_STRIP_H;
        for (int i = 0; i < LCD_H_RES * h; i++) strip[i] = (y + i) & 0xFFFF;
        Serial.write((const uint8_t *)strip, LCD_H_RES * h * 2);
    }
    Serial.println("\r[SCR] done (mock)");
    free(strip);
    lvglSetScreenEnabled(prev);
    return true;
}

/* ===== Strip-based RGB565 Screenshot (v5.5: static buf + reduced iters) =====
 *
 * RAM: 24KB static strip buffer (.bss) + 2×48KB LVGL draw buffers ≈ 120KB.
 *
 * Pipeline:
 *  1. spi2_lock()     — lock SPI2 bus AND drain any in-flight QSPI transaction
 *                        (lock blocks until current holder releases → queue is drained)
 *  2. Flush override   — intercept LVGL flush, copy pixels to strip buf (no QSPI write!)
 *  3. Strip loop:      — lv_obj_invalidate() → lv_timer_handler×5 → Serial.write()
 *                        LVGL PARTIAL mode processes ALL dirty tiles within each
 *                        timer_handler call; 5 iters gives margin for multi-batch edge cases.
 *  4. LCD recovery:    — PMU power-cycle LCD → esp_lcd_panel_init() → re-render
 *  5. spi2_unlock()    — release SPI2 bus
 *
 * WHY NO QSPI WRITES during step 3:
 *   Sending QSPI commands + Serial I/O in the same critical section causes
 *   DMA arbitration conflicts on SPI2/CPU bus matrix → SH8601 state corruption.
 *   By skipping QSPI entirely during capture, we avoid the root cause.
 *
 * WHY PMU POWER-CYCLE recovery instead of esp_lcd_panel_reset():
 *   Our board has reset_gpio_num = -1 (no dedicated RST pin). LCD reset is
 *   done via PMU power toggle: OFF→delay→ON→init. This fully drains SH8601's
 *   internal state and re-runs the QSPI init sequence cleanly.
 */
static bool screenshotCaptureRaw(void) {
    size_t stripBytes = LCD_H_RES * SCREENSHOT_STRIP_H * 2;

    /* ── Phase 1: Lock SPI2 (also drains in-flight QSPI transactions) ── */
    if (!spi2_lock()) {
        Serial.println("[SCR] ERR: SPI2 lock failed (bus busy)");
        return false;
    }

    lvglSetScreenEnabled(true);
    lvglSetFlushOverride(scrFlushCb);

    /* ── Phase 2: Send header (USB CDC, no baud switching needed) ── */
    uint8_t header[6] = {
        0xCC, 0x53,
        (uint8_t)(LCD_H_RES & 0xFF), (uint8_t)((LCD_H_RES >> 8) & 0xFF),
        (uint8_t)(LCD_V_RES & 0xFF), (uint8_t)((LCD_V_RES >> 8) & 0xFF)
    };
    Serial.write(header, 6);
    Serial.flush();

    /* ── Phase 3: Strip capture loop ──
     * Each iteration: invalidate full screen, LVGL re-renders ALL dirty tiles,
     * flush override copies only the current strip's Y range, rest is discarded. */
    lv_obj_t *scr = lv_screen_active();

    for (int y = 0; y < LCD_V_RES; y += SCREENSHOT_STRIP_H) {
        int h = (y + SCREENSHOT_STRIP_H > LCD_V_RES) ? LCD_V_RES - y : SCREENSHOT_STRIP_H;

        s_stripYStart = y;
        s_stripYEnd   = y + h;
        memset(s_scr_buf, 0, stripBytes);

        lv_obj_invalidate(scr);
        for (int i = 0; i < SCR_STRIP_ITER; i++) {
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        /* Send strip pixel data (RGB565 raw, USB CDC max throughput) */
        Serial.write((const uint8_t *)s_scr_buf, LCD_H_RES * h * 2);
    }

    /* ── Phase 4: Cleanup flush override ── */
    s_stripYStart = s_stripYEnd = 0;
    lvglSetFlushOverride(NULL);

    /* ── Phase 5: LCD QSPI State Recovery ──
     * SH8601's internal CASET/RASET address registers are stale because
     * no QSPI commands were sent during the screenshot. A PMU power-cycle
     * drains SH8601 state, then esp_lcd_panel_init() re-runs the full
     * init command sequence (Sleep Out, registers, Display ON).
     *
     * Screen will flash black for ~300ms during the power cycle —
     * acceptable for a debug feature. */
    powerManager.enableLcdPower(false);
    delay(150);
    powerManager.enableLcdPower(true);
    delay(150);

    esp_lcd_panel_handle_t panel = display.getPanelHandle();
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    /* Re-apply runtime display settings that panel_init resets to defaults */
    uint8_t savedBrightness = display.getBrightness();
    if (savedBrightness != 100) {
        display.setBrightness(savedBrightness);
    }
    uint8_t savedRotation = display.getRotation();
    if (savedRotation != 0) {
        display.setRotation(savedRotation);
    }

    /* ── Phase 6: Force LVGL to re-render current screen onto fresh panel ── */
    lvglSetScreenEnabled(true);
    lv_obj_invalidate(lv_screen_active());
    for (int i = 0; i < POST_RECOVERY_ITER; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    spi2_unlock();

    Serial.println("[SCR] done");
    return true;
}

/* Strip-based PPM screenshot (same static-buf + SPI2-locked + recovery strategy).
 * PPM format: ASCII header "P6\nW H\n255\n" + raw RGB888 pixels. */
static bool screenshotCapturePPM(void) {
    size_t stripBytes = LCD_H_RES * SCREENSHOT_STRIP_H * 2;

    if (!spi2_lock()) {
        Serial.println("[SCR] ERR: SPI2 lock failed");
        return false;
    }

    lvglSetScreenEnabled(true);
    lvglSetFlushOverride(scrFlushCb);

    /* PPM header — USB CDC, no baud switching needed */
    Serial.printf("P6\n%d %d\n255\n", LCD_H_RES, LCD_V_RES);
    Serial.flush();

    lv_obj_t *scr = lv_screen_active();
    uint8_t rgbRow[LCD_H_RES * 3];  /* stack buffer 480*3=1440 bytes, safe */

    for (int y = 0; y < LCD_V_RES; y += SCREENSHOT_STRIP_H) {
        int h = (y + SCREENSHOT_STRIP_H > LCD_V_RES) ? LCD_V_RES - y : SCREENSHOT_STRIP_H;

        s_stripYStart = y;
        s_stripYEnd   = y + h;
        memset(s_scr_buf, 0, stripBytes);

        lv_obj_invalidate(scr);
        for (int i = 0; i < SCR_STRIP_ITER; i++) {
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        /* Convert strip RGB565 → RGB888 and send row by row */
        for (int row = 0; row < h; row++) {
            for (int x = 0; x < LCD_H_RES; x++) {
                uint16_t c = s_scr_buf[row * LCD_H_RES + x];
                rgbRow[x*3+0] = ((c >> 11) & 0x1F) << 3;
                rgbRow[x*3+1] = ((c >> 5)  & 0x3F) << 2;
                rgbRow[x*3+2] = (c          & 0x1F) << 3;
            }
            Serial.write(rgbRow, LCD_H_RES * 3);
        }
    }

    /* Cleanup */
    s_stripYStart = s_stripYEnd = 0;
    lvglSetFlushOverride(NULL);

    /* LCD QSPI Recovery — same PMU power-cycle as raw version */
    powerManager.enableLcdPower(false);
    delay(150);
    powerManager.enableLcdPower(true);
    delay(150);

    esp_lcd_panel_handle_t panel = display.getPanelHandle();
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    uint8_t savedBrightness = display.getBrightness();
    if (savedBrightness != 100)  display.setBrightness(savedBrightness);
    uint8_t savedRotation = display.getRotation();
    if (savedRotation != 0)      display.setRotation(savedRotation);

    lvglSetScreenEnabled(true);
    lv_obj_invalidate(lv_screen_active());
    for (int i = 0; i < POST_RECOVERY_ITER; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    spi2_unlock();

    Serial.println("[SCR] PPM done");
    return true;
}

/* ===== Strip-based BMP Screenshot to SD card (v5.x) =====
 *
 * Writes 480×480 24-bit BMP directly to SD card.
 * Format: top-down (negative biHeight), BGR byte order, no padding (480×3 = 1440 ≡ 0 mod 4).
 *
 * SPI2 BUS STRATEGY (v5.5 fix):
 *   - spi2_lock() acquired before LVGL render phase → prevents QSPI vs SPI conflict
 *   - spi2_unlock() released before each strip's fwrite() → allows SD card SPI access
 *   - spi2_lock() re-acquired after fwrite() → ready for next strip's LVGL render
 *   - Without this lock/unlock per strip, the SPI2 bus stays in SD mode after fwrite(),
 *     and subsequent lv_timer_handler() renders produce zero pixels (all-black strips).
 *
 * Output: /sdcard/ChronoCube/screenshots/scr_0001.bmp, ... (auto-increment counter). */
static bool screenshotToBMP(void) {
    size_t stripBytes = LCD_H_RES * SCREENSHOT_STRIP_H * 2;

    /* ── Generate filename with auto-increment counter ── */
    static int s_scrCounter = 0;
    char filepath[56];
    if (s_scrCounter == 0) {
        /* First call: scan existing scr_*.bmp to avoid overwriting */
        for (int i = 1; i <= 9999; i++) {
            snprintf(filepath, sizeof(filepath), "/sdcard/ChronoCube/screenshots/scr_%04d.bmp", i);
            FILE *check = fopen(filepath, "rb");
            if (!check) { s_scrCounter = i; break; }
            fclose(check);
        }
        if (s_scrCounter == 0) s_scrCounter = 1;  /* all taken? start at 1 anyway */
    }
    snprintf(filepath, sizeof(filepath), "/sdcard/ChronoCube/screenshots/scr_%04d.bmp", s_scrCounter);

    /* ── Phase 1: Open file & write BMP header ── */
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        Serial.printf("[SCR] ERR: cannot open %s\n", filepath);
        return false;
    }

    uint32_t w = LCD_H_RES, h = LCD_V_RES;
    uint32_t rowSize  = w * 3;                    /* 480×3 = 1440, already 4-byte aligned */
    uint32_t pixelSize = rowSize * h;              /* 691,200 bytes */
    uint32_t fileSize  = 54 + pixelSize;           /* 691,254 bytes */
    int32_t  bmpHeight = -(int32_t)h;              /* negative = top-down */

    /* BMP File Header (14 bytes) */
    uint8_t bmpHdr[54] = {0};
    bmpHdr[0]  = 'B';  bmpHdr[1]  = 'M';                                   /* signature */
    bmpHdr[2]  = (uint8_t)(fileSize);                                       /* file size LE */
    bmpHdr[3]  = (uint8_t)(fileSize >> 8);
    bmpHdr[4]  = (uint8_t)(fileSize >> 16);
    bmpHdr[5]  = (uint8_t)(fileSize >> 24);
    bmpHdr[10] = 54;                                                         /* pixel offset */

    /* DIB Header (40 bytes) — starts at offset 14 */
    bmpHdr[14] = 40;                                                         /* header size */
    bmpHdr[18] = (uint8_t)(w);  bmpHdr[19] = (uint8_t)(w >> 8);            /* width */
    bmpHdr[22] = (uint8_t)(bmpHeight);                                      /* height LE (negative) */
    bmpHdr[23] = (uint8_t)(bmpHeight >> 8);
    bmpHdr[24] = (uint8_t)(bmpHeight >> 16);
    bmpHdr[25] = (uint8_t)(bmpHeight >> 24);
    bmpHdr[26] = 1;                                                          /* planes */
    bmpHdr[28] = 24;                                                         /* bpp */
    /* compression = 0 (BI_RGB), imageSize = 0 (valid for BI_RGB) — already zero */
    /* DPI = 72 → 2835 pixels/meter */
    bmpHdr[38] = 0x13; bmpHdr[39] = 0x0B;                                  /* 2835 LE */
    bmpHdr[42] = 0x13; bmpHdr[43] = 0x0B;                                  /* 2835 LE */

    fwrite(bmpHdr, 1, 54, f);

    /* ── Phase 2: Lock SPI2 for LVGL render & set flush override ── */
    if (!spi2_lock()) {
        Serial.println("[SCR] ERR: SPI2 lock failed");
        fclose(f);
        return false;
    }

    lvglSetScreenEnabled(true);
    lvglSetFlushOverride(scrFlushCb);

    /* Drain any in-flight QSPI transactions before SD card access */
    delay(30);

    /* ── Phase 3: Strip capture → RGB565→BGR888 → SD card ──
     * Each strip: lock SPI2 → LVGL render → unlock → fwrite to SD → re-lock */
    lv_obj_t *scr = lv_screen_active();
    uint8_t rgbRow[LCD_H_RES * 3];   /* 1440 bytes on stack */

    for (int y = 0; y < (int)LCD_V_RES; y += SCREENSHOT_STRIP_H) {
        int stripH = (y + SCREENSHOT_STRIP_H > (int)LCD_V_RES) ? (int)LCD_V_RES - y : SCREENSHOT_STRIP_H;

        s_stripYStart = y;
        s_stripYEnd   = y + stripH;
        memset(s_scr_buf, 0, stripBytes);

        lv_obj_invalidate(scr);
        for (int i = 0; i < SCR_STRIP_ITER; i++) {
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        /* Release SPI2 lock so SD card fwrite() can use the bus */
        spi2_unlock();

        /* Convert RGB565 → BGR888 (BMP byte order) and write row by row */
        for (int row = 0; row < stripH; row++) {
            uint16_t *src = &s_scr_buf[row * LCD_H_RES];
            for (int x = 0; x < (int)LCD_H_RES; x++) {
                uint16_t c = src[x];
                rgbRow[x*3+0] = (c          & 0x1F) << 3;   /* Blue  */
                rgbRow[x*3+1] = ((c >> 5)  & 0x3F) << 2;   /* Green */
                rgbRow[x*3+2] = ((c >> 11) & 0x1F) << 3;   /* Red   */
            }
            fwrite(rgbRow, 1, LCD_H_RES * 3, f);
        }

        /* Re-acquire SPI2 lock for next strip's LVGL render (or for recovery) */
        if (y + SCREENSHOT_STRIP_H < (int)LCD_V_RES) {
            if (!spi2_lock()) {
                Serial.println("[SCR] ERR: SPI2 lock lost mid-capture");
                break;
            }
        } else {
            /* Last strip: re-lock for recovery phase */
            if (!spi2_lock()) {
                Serial.println("[SCR] ERR: SPI2 lock lost before recovery");
            }
        }
    }

    /* ── Phase 4: Cleanup ── */
    s_stripYStart = s_stripYEnd = 0;
    lvglSetFlushOverride(NULL);
    fclose(f);

    /* ── Phase 5: LCD QSPI State Recovery (same as raw/PPM) ── */
    powerManager.enableLcdPower(false);
    delay(150);
    powerManager.enableLcdPower(true);
    delay(150);

    esp_lcd_panel_handle_t panel = display.getPanelHandle();
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    uint8_t savedBrightness = display.getBrightness();
    if (savedBrightness != 100)  display.setBrightness(savedBrightness);
    uint8_t savedRotation = display.getRotation();
    if (savedRotation != 0)      display.setRotation(savedRotation);

    lvglSetScreenEnabled(true);
    lv_obj_invalidate(lv_screen_active());
    for (int i = 0; i < POST_RECOVERY_ITER; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    spi2_unlock();

    Serial.printf("[SCR] %s (%lu bytes)\n", filepath, fileSize);
    s_scrCounter++;   /* next file gets next number */
    return true;
}

static void onCmdScreenshotSD(void) {
    if (!screenshotToBMP()) Serial.println("[DBG] Screenshot SD failed.");
}

static void onCmdScreenshot(void) {
    if (!screenshotCaptureRaw()) Serial.println("[DBG] Screenshot failed.");
}

static void onCmdScreenshotPPM(void) {
    if (!screenshotCapturePPM()) Serial.println("[DBG] Screenshot PPM failed.");
}

static void onCmdScreen(const char *name) {
    if (!lvglIsScreenEnabled()) { lvglSetScreenEnabled(true); }
    static const struct { const char *name; ui_screen_t id; SystemState st; } screen_map[] = {
        {"standby", UI_SCREEN_STANDBY, STATE_STANDBY},
        {"focus",   UI_SCREEN_FOCUS,   STATE_DEEP_FOCUS},
        {"pause",   UI_SCREEN_PAUSE,   STATE_PAUSE},
        {"rest",    UI_SCREEN_REST,    STATE_DEEP_REST},
        {"emotion", UI_SCREEN_EMOTION, STATE_EMOTION_PICK},
        {"summary", UI_SCREEN_SUMMARY, STATE_STANDBY},
        {"locked",  UI_SCREEN_LOCKED,  STATE_STANDBY},
        {"lowbat",  UI_SCREEN_LOW_BATTERY, STATE_STANDBY},
        {"restend", UI_SCREEN_REST_END, STATE_DEEP_REST},
    };
    for (size_t i = 0; i < sizeof(screen_map)/sizeof(screen_map[0]); i++) {
        if (tokMatch(name, screen_map[i].name)) {
            ui_screen_t sid = screen_map[i].id;

            /* 设置状态机状态，避免主循环刷回原屏幕 */
            SystemState st = screen_map[i].st;
            if (st == STATE_STANDBY)        timerManager.enterStandby();
            else if (st == STATE_PAUSE)     timerManager.enterPause();
            else if (st == STATE_EMOTION_PICK) timerManager.enterEmotionPick();
            else if (st == STATE_DEEP_REST) timerManager.enterRest(STATE_DEEP_FOCUS);
            else if (st == STATE_DEEP_FOCUS) timerManager.enterFocus(STATE_DEEP_FOCUS, true);

            /* 弹窗类不通过 showScreenForState，直接弹出 */
            if (sid == UI_SCREEN_SUMMARY) ui_show_total_popup("00:00", "00:00", "00:00");
            else if (sid == UI_SCREEN_LOW_BATTERY) ui_show_low_battery_pct(powerManager.getBatteryPercent());
            else if (sid == UI_SCREEN_REST_END) {
                ui_show_screen(UI_SCREEN_REST_END);
                ui_show_rest_end("即将继续专注 · 5s");
            }
            else if (sid == UI_SCREEN_LOCKED) {
                keys.setLocked(true);
                lvglBridge_setLocked(true);
            } else {
                lvglBridge_showScreenForState(timerManager.getState());
            }
            Serial.printf("[DBG] Screen '%s'\n", name);
            return;
        }
    }
    Serial.printf("Unknown screen: %s\n", name);
}

static void onCmdRot(const char *arg) {
    uint8_t idx = (uint8_t)atol(arg);
    static const uint8_t madctl_vals[4] = {0x30, 0xA0, 0xC0, 0x60};
    static const uint16_t degrees[4] = {0, 90, 180, 270};
    if (idx < 4) {
        display.setRotation(degrees[idx]);
        touch.setRotation(degrees[idx]);
        Serial.printf("[ROT] idx=%u MADCTL=0x%02X deg=%u\n", idx, madctl_vals[idx], degrees[idx]);
    } else {
        Serial.println("[ROT] usage: rot <0-3>  (0=0deg 1=90deg 2=180deg 3=270deg)");
    }
}

static void onCmdMadctl(const char *arg) {
    uint8_t val = (uint8_t)strtol(arg, NULL, 16);
    display.testMADCTL(val);
    Serial.printf("[MADCTL] sent raw 0x%02X — observe screen direction & colors\n", val);
    Serial.println("[MADCTL] hint: 0x30=0deg  0x60=270?  0xA0=90?  0xC0=180?");
}

#endif /* USE_LVGL */

/* ==================== Watchdog (模块级活动监控) ==================== */

#define WATCHDOG_MAX_ENTRIES 12

struct WatchdogSlot {
  const char *name;
  unsigned long lastMs;
};

static WatchdogSlot s_wd_slots[WATCHDOG_MAX_ENTRIES];
static int s_wd_count = 0;

void debugWatchdog_poke(const char *module) {
  if (!module) return;
  // 查找已有槽位
  for (int i = 0; i < s_wd_count; i++) {
    if (strcmp(s_wd_slots[i].name, module) == 0) {
      s_wd_slots[i].lastMs = millis();
      return;
    }
  }
  // 新槽位
  if (s_wd_count < WATCHDOG_MAX_ENTRIES) {
    s_wd_slots[s_wd_count].name   = module;
    s_wd_slots[s_wd_count].lastMs = millis();
    s_wd_count++;
  }
}

void debugWatchdog_dump(void) {
  unsigned long now = millis();
  Serial.println("=== WATCHDOG: Module Activity Monitor ===");
  Serial.println("  Module          Last(ms ago)  Status");
  Serial.println("  ──────────────  ────────────  ──────");
  for (int i = 0; i < s_wd_count; i++) {
    unsigned long ago = now - s_wd_slots[i].lastMs;
    const char *status;
    if      (ago < 5000)   status = "OK";
    else if (ago < 30000)  status = "SLOW";
    else if (ago < 60000)  status = "STALL";
    else                   status = "HUNG!";
    Serial.printf("  %-14s  %8lu ms    %s\n",
                  s_wd_slots[i].name, ago, status);
  }
  if (s_wd_count == 0) {
    Serial.println("  (no modules registered — add debugWatchdog_poke() in loop())");
  }
  Serial.println("  Legend: OK(<5s) SLOW(5-30s) STALL(30-60s) HUNG(>60s)");
}

/* ==================== Loop 性能统计 ==================== */

static unsigned long s_loopLastMs   = 0;
static unsigned long s_loopMinMs    = 0xFFFFFFFF;
static unsigned long s_loopMaxMs    = 0;
static unsigned long s_loopTotalMs  = 0;
static unsigned long s_loopCount    = 0;
static unsigned long s_loopOver5ms  = 0;     // >5ms 的次数（说明有阻塞）
static unsigned long s_loopOver20ms = 0;     // >20ms 的次数（严重阻塞）

void debugLoop_record(unsigned long now) {
  if (s_loopLastMs == 0) { s_loopLastMs = now; return; }
  unsigned long dt = now - s_loopLastMs;
  s_loopLastMs = now;

  if (dt > 2000) return;     // 排除首次调用或长时间暂停（如截图）
  s_loopCount++;
  s_loopTotalMs += dt;
  if (dt < s_loopMinMs) s_loopMinMs = dt;
  if (dt > s_loopMaxMs) s_loopMaxMs = dt;
  if (dt > 5)  s_loopOver5ms++;
  if (dt > 20) s_loopOver20ms++;
}

static void onCmdLoop(void) {
  unsigned long avg = s_loopCount ? (s_loopTotalMs / s_loopCount) : 0;
  Serial.println("=== LOOP TIMING STATS ===");
  Serial.printf("  Iterations : %lu\n", s_loopCount);
  Serial.printf("  Current    : %lu ms\n", millis() - s_loopLastMs);
  Serial.printf("  Min / Max  : %lu / %lu ms\n", s_loopMinMs, s_loopMaxMs);
  Serial.printf("  Average    : %lu ms\n", avg);
  Serial.printf("  >5ms  count: %lu (%.1f%%)\n",
    s_loopOver5ms, s_loopCount ? (float)s_loopOver5ms * 100.0f / s_loopCount : 0);
  Serial.printf("  >20ms count: %lu (%.1f%%)\n",
    s_loopOver20ms, s_loopCount ? (float)s_loopOver20ms * 100.0f / s_loopCount : 0);
  if (s_loopOver20ms > 0) {
    Serial.println("  ⚠ >20ms iterations detected — possible blocking code!");
    Serial.println("     Check: delay(), long I2C ops, SD card access, WiFi");
  }
}

static void onCmdWatchdog(void) { debugWatchdog_dump(); }

/* ==================== 公共 API ==================== */

void debugConsole_tick(void) {
    // 非阻塞逐字节读取，避免 readBytesUntil 在数据不完整时阻塞 loop
    static char line[CMD_BUF_SIZE];
    static int  linePos = 0;
    static unsigned long lastRxMs = 0;

    while (Serial.available()) {
        char c = Serial.read();
        lastRxMs = millis();

        if (c == '\r') continue;  // 忽略回车

        if (c == '\n') {
            // 完整一行到达 → 处理
            if (linePos == 0) continue;  // 空行跳过
            line[linePos] = '\0';
            linePos = 0;

            const char *argv[CMD_ARGS_MAX];
            int argc = tokenize(line, argv, CMD_ARGS_MAX);
            if (argc == 0) { printPrompt(); continue; }

            const char *cmd = argv[0];
            Serial.printf(">>> %s\n", line);

            if (tokMatch(cmd, "help"))           onCmdHelp();
            else if (tokMatch(cmd, "info"))      onCmdInfo();
            else if (tokMatch(cmd, "event"))    onCmdEvent(argc > 1 ? argv[1] : NULL);
            else if (tokMatch(cmd, "key")) {
                if (argc < 2) Serial.println("Usage: key <type>");
                else onCmdKey(argv[1]);
            }
            else if (tokMatch(cmd, "pose")) {
                if (argc < 2) Serial.println("Usage: pose <face>");
                else onCmdPose(argv[1]);
            }
            else if (tokMatch(cmd, "state")) {
                if (argc < 2) Serial.println("Usage: state <name>");
                else onCmdState(argv[1]);
            }
            else if (tokMatch(cmd, "lock")) {
                if (argc < 2) Serial.println("Usage: lock 0|1");
                else onCmdLock(argv[1]);
            }
            else if (tokMatch(cmd, "mute"))       onCmdMute();
            else if (tokMatch(cmd, "beep")) {
                onCmdBeep(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);
            }
            else if (tokMatch(cmd, "mclk"))      onCmdMclk();
            else if (tokMatch(cmd, "timer")) {
                onCmdTimer(argc > 1 ? argv[1] : NULL);
            }
            else if (tokMatch(cmd, "time")) {
                onCmdTime(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);
            }
            /* ---- Network/Storage/RTC commands ---- */
            else if (tokMatch(cmd, "wifi"))     onCmdWifi();
            else if (tokMatch(cmd, "sd"))       onCmdSd();
            else if (tokMatch(cmd, "net"))      onCmdNet();
            else if (tokMatch(cmd, "flush")) {
                onCmdFlush(argc > 1 ? argv[1] : NULL, argc, argv);
            }
            else if (tokMatch(cmd, "rtc"))      onCmdRtc();
            /* ---- Diagnostic commands ---- */
            else if (tokMatch(cmd, "heap"))       onCmdHeap();
            else if (tokMatch(cmd, "crash"))      onCmdCrash();
            else if (tokMatch(cmd, "i2c"))        onCmdI2c();
            else if (tokMatch(cmd, "i2c_dump")) {
                if (argc < 3) Serial.println("Usage: i2c_dump <addr_hex> <start_reg_hex> [count]");
                else onCmdI2cDump(argv[1], argv[2], argc > 3 ? argv[3] : NULL);
            }
            else if (tokMatch(cmd, "task"))       onCmdTask();
            else if (tokMatch(cmd, "uptime"))     onCmdUptime();
            else if (tokMatch(cmd, "stress")) {
                if (argc < 2) Serial.println("Usage: stress <state|beep|all> [n]");
                else onCmdStress(argv[1], argc > 2 ? argv[2] : NULL);
            }
            else if (tokMatch(cmd, "watchdog"))  onCmdWatchdog();
            else if (tokMatch(cmd, "loop"))      onCmdLoop();
            else if (tokMatch(cmd, "fontcache"))  onCmdFontCache();
            else if (tokMatch(cmd, "powersave")) {
                if (argc < 2) {
                    Serial.printf("[DBG] powerSaveEnabled = %s\n",
                        configGetRuntime().powerSaveEnabled ? "true (熄屏)" : "false (常亮)");
                } else {
                    bool en = (atoi(argv[1]) != 0);
                    configSetPowerSave(en);
                    Serial.printf("[DBG] Power save %s (screen-off %s)\n",
                        en ? "ON" : "OFF", en ? "enabled" : "disabled");
                }
            }
            else if (tokMatch(cmd, "deadpix")) {
                if (argc < 2) Serial.println("Usage: deadpix <red|green|blue|white|black|...>");
                else onCmdDeadPix(argv[1]);
            }
#ifdef USE_LVGL
            else if (tokMatch(cmd, "screenshot"))      onCmdScreenshot();
            else if (tokMatch(cmd, "screenshot_ppm"))  onCmdScreenshotPPM();
            else if (tokMatch(cmd, "screenshot_sd"))   onCmdScreenshotSD();
            else if (tokMatch(cmd, "screen")) {
                if (argc < 2) Serial.println("Usage: screen <name>");
                else onCmdScreen(argv[1]);
            }
            else if (tokMatch(cmd, "rot")) {
                if (argc < 2) Serial.println("Usage: rot <0-3>");
                else onCmdRot(argv[1]);
            }
            else if (tokMatch(cmd, "madctl")) {
                if (argc < 2) Serial.println("Usage: madctl <hex>  (e.g. madctl 30)");
                else onCmdMadctl(argv[1]);
            }
            else if (tokMatch(cmd, "setbright")) {
                if (argc < 2) Serial.println("Usage: setbright <0-100>");
                else {
                    int val = atoi(argv[1]);
                    if (val < 0) val = 0;
                    if (val > 100) val = 100;
                    display.setBrightness((uint8_t)val);
                    Serial.printf("[DBG] Brightness set to %d%%\n", val);
                }
            }
#endif
            else if (tokMatch(cmd, "setvolume")) {
                if (argc < 2) Serial.println("Usage: setvolume <hex>  (e.g. setvolume C0)");
                else {
                    uint8_t val = (uint8_t)strtol(argv[1], NULL, 16);
                    audio.setVolume(val);
                    uint8_t rd = 0;
                    if (I2CBus::readReg8(0x18, 0x32, &rd) == 0) {
                        bool match = (rd == val);
                        Serial.printf("[DBG] Volume set=0x%02X readback=0x%02X %s\n",
                                      val, rd, match ? "VERIFIED" : "MISMATCH!");
                    } else {
                        Serial.printf("[DBG] Volume set=0x%02X (readback failed)\n", val);
                    }
                }
            }
            else if (tokMatch(cmd, "volume")) {
                if (argc < 2) Serial.println("Usage: volume <0-100>  (e.g. volume 50)");
                else {
                    int val = atoi(argv[1]);
                    if (val < 0) val = 0;
                    if (val > 100) val = 100;
                    // 映射 0-100% → ES8311 0x32 寄存器值
                    // 0x00=静音, 0xC0=-0.5dB(默认), 0xFF=最大
                    uint8_t reg = (uint8_t)(val * 0xFF / 100);
                    audio.setVolume(reg);
                    Serial.printf("[DBG] Volume set to %d%% (reg=0x%02X)\n", val, reg);
                }
            }
            else if (tokMatch(cmd, "audiodebug")) {
                if (argc < 2) Serial.println("Usage: audiodebug 0|1");
                else audio.setDebugVerbose(atoi(argv[1]) != 0);
            }
            else if (tokMatch(cmd, "audiostat")) {
                audio.debugHealthDump();
            }
            else {
                Serial.printf("[DBG] unknown: %s (try 'help')\n", cmd);
            }

            printPrompt();
        } else if (linePos < CMD_BUF_SIZE - 1) {
            // 缓冲非换行字符
            line[linePos++] = c;
        }
        // else: 行太长，丢弃多余字符（等换行时重置）
    }

    // 超时保护：超过 5 秒无输入则丢弃半行缓冲区
    if (linePos > 0 && millis() - lastRxMs > 5000) {
        linePos = 0;
        Serial.println("\n[DBG] (input timeout, buffer cleared)");
        printPrompt();
    }
}

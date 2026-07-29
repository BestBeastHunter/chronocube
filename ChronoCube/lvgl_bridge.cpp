#include "config.h"

#ifdef USE_LVGL

#include "lvgl_bridge.h"
#include "ui_lvgl_pro.h"
#include "lv_port_disp.h"
#include <stdio.h>
#include <time.h>

/* ==================== 状态 → 屏幕 + 模式映射 ==================== */

void lvglBridge_showScreenForState(SystemState state) {
  if (!lvglIsScreenEnabled()) return;

  switch (state) {
    case STATE_STANDBY:
      ui_show_screen(UI_SCREEN_STANDBY);
      break;

    case STATE_DEEP_FOCUS:
      ui_show_screen(UI_SCREEN_FOCUS);
      ui_set_mode(UI_MODE_DEEP_FOCUS);
      break;
    case STATE_LIGHT_WORK:
      ui_show_screen(UI_SCREEN_FOCUS);
      ui_set_mode(UI_MODE_LIGHT_WORK);
      break;
    case STATE_STUDY:
      ui_show_screen(UI_SCREEN_FOCUS);
      ui_set_mode(UI_MODE_STUDY);
      break;

    case STATE_PAUSE:
      ui_show_screen(UI_SCREEN_PAUSE);
      break;

    case STATE_DEEP_REST:
      ui_show_screen(UI_SCREEN_REST);
      ui_set_mode(UI_MODE_DEEP_REST);
      break;
    case STATE_LIGHT_REST:
      ui_show_screen(UI_SCREEN_REST);
      ui_set_mode(UI_MODE_LIGHT_REST);
      break;
    case STATE_STUDY_REST:
      ui_show_screen(UI_SCREEN_REST);
      ui_set_mode(UI_MODE_STUDY_REST);
      break;

    case STATE_EMOTION_PICK:
      ui_show_screen(UI_SCREEN_EMOTION);
      break;

    default:
      ui_show_screen(UI_SCREEN_STANDBY);
      break;
  }
}

/* ==================== 状态 → 颜色映射 ==================== */

lv_color_t colorForState(SystemState state) {
  switch (state) {
    case STATE_DEEP_FOCUS:    return ui_mode_color(UI_MODE_DEEP_FOCUS);
    case STATE_LIGHT_WORK:    return ui_mode_color(UI_MODE_LIGHT_WORK);
    case STATE_STUDY:         return ui_mode_color(UI_MODE_STUDY);
    case STATE_PAUSE:         return CP_RED;
    case STATE_EMOTION_PICK:  return ui_mode_color(UI_MODE_LIGHT_WORK);
    case STATE_DEEP_REST:     return ui_mode_color(UI_MODE_DEEP_REST);
    case STATE_LIGHT_REST:    return ui_mode_color(UI_MODE_LIGHT_REST);
    case STATE_STUDY_REST:    return ui_mode_color(UI_MODE_STUDY_REST);
    default:                  return CP_TEXT;
  }
}

/* ==================== 计时屏数据更新（每秒） ==================== */

void lvglBridge_updateData(unsigned long remainSec, int pct, SystemState state) {

  /* 仅计时相关屏幕更新 */
  ui_screen_t scr = ui_get_current_screen();
  if (scr != UI_SCREEN_FOCUS && scr != UI_SCREEN_REST && scr != UI_SCREEN_PAUSE)
    return;

  /* 格式化时间文本 — 始终 MM:SS，匹配设计稿 */
  char timeStr[16];
  unsigned long total = remainSec;
  unsigned long m = total / 60;
  unsigned long s = total % 60;
  snprintf(timeStr, sizeof(timeStr), "%02lu:%02lu", m, s);

  lv_color_t color = colorForState(state);

  ui_set_timer_text(color, timeStr);

  /* 进度仅计时屏有效 */
  if (scr == UI_SCREEN_FOCUS || scr == UI_SCREEN_REST)
    ui_set_progress(pct);
}

/* ==================== 待机仪表板数据 ==================== */

/* 格式化时间：始终 HH:MM:SS（用于 S0/S9 累计时长） */
static void fmtHMS_str(char *buf, size_t len, unsigned long secs) {
  unsigned long h = secs / 3600;
  unsigned long m = (secs % 3600) / 60;
  unsigned long s = secs % 60;
  snprintf(buf, len, "%02lu:%02lu:%02lu", h, m, s);
}

void lvglBridge_updateStandby(unsigned long deepSec,
                               unsigned long lightSec,
                               unsigned long studySec,
                               unsigned long pauseSec) {
  char deep[16], light[16], study[16], pause[16];
  fmtHMS_str(deep,  sizeof(deep),  deepSec);
  fmtHMS_str(light, sizeof(light), lightSec);
  fmtHMS_str(study, sizeof(study), studySec);
  fmtHMS_str(pause, sizeof(pause), pauseSec);
  ui_set_standby_data(deep, light, study, pause);
}

/* 格式化锁屏日期：周三 07.22 第30周；无 RTC 时返回占位 */
void lvglBridge_getLockDateStr(char *buf, size_t len) {
  time_t now = time(nullptr);
  struct tm *tm = localtime(&now);
  if (tm && now > 1000000000) {
    const char *weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    char weekStr[4] = {0};
    strftime(weekStr, sizeof(weekStr), "%V", tm);
    snprintf(buf, len, "%s %02d.%02d 第%s周",
             weekdays[tm->tm_wday], tm->tm_mon + 1, tm->tm_mday, weekStr);
  } else {
    snprintf(buf, len, "周五 07.17 第29周");
  }
}

/* ==================== 锁定控制 ==================== */

void lvglBridge_setLocked(bool locked) {
  if (locked) {
    /* 设置锁屏时钟（使用系统时间） */
    time_t now = time(nullptr);
    struct tm *tm = localtime(&now);
    char timeBuf[8];
    if (tm && now > 1000000000) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else {
      snprintf(timeBuf, sizeof(timeBuf), "00:00");
    }

    /* 日期：有系统时间用系统时间，否则占位 */
    char dateBuf[32];
    lvglBridge_getLockDateStr(dateBuf, sizeof(dateBuf));

    ui_set_lock_clock(timeBuf, dateBuf);
    ui_show_screen(UI_SCREEN_LOCKED);
  }
  /* 解锁时由 lvglBridge_showScreenForState 处理 */
}

/* ==================== S9 总时长弹窗 ==================== */

void lvglBridge_showSummaryPopup(unsigned long effectiveSec,
                                  unsigned long ineffectiveSec,
                                  unsigned long restSec) {
  char eff[16], ineff[16], rest[16];
  fmtHMS_str(eff,   sizeof(eff),   effectiveSec);
  fmtHMS_str(ineff, sizeof(ineff), ineffectiveSec);
  fmtHMS_str(rest,  sizeof(rest),  restSec);
  ui_show_total_popup(eff, ineff, rest);
}

#endif /* USE_LVGL */

#ifndef UI_LVGL_PRO_H_
#define UI_LVGL_PRO_H_
/**
 * @file ui_lvgl_pro.h
 * ChronoCube LVGL UI — 公共 API (v5.3 新设计)
 *
 * 配色: Catppuccin Mocha
 * 字体: cn_font (24×24 中文), Montserrat (12~36 英文)
 * 原型参考: chronocube-s1-focus-screen/pages/*.html
 */

#ifdef USE_LVGL
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 配色系统 (Catppuccin Mocha, RGB565) ==================== */
#define CP_MANTLE    lv_color_hex(0x181825)
#define CP_BASE      lv_color_hex(0x1E1E2E)
#define CP_TEXT      lv_color_hex(0xCDD6F4)
#define CP_SUBTEXT1  lv_color_hex(0xBAC2DE)
#define CP_SUBTEXT0  lv_color_hex(0xA6ADC8)
#define CP_SURFACE0  lv_color_hex(0x313244)
#define CP_SURFACE1  lv_color_hex(0x45475A)
#define CP_GREEN     lv_color_hex(0xA6E3A1)
#define CP_PEACH     lv_color_hex(0xFAB387)
#define CP_MAUVE     lv_color_hex(0xCBA6F7)
#define CP_YELLOW    lv_color_hex(0xF9E2AF)
#define CP_RED       lv_color_hex(0xF38BA8)
#define CP_MAROON    lv_color_hex(0xEBA0AC)
#define CP_TEAL      lv_color_hex(0x94E2D5)  // 休息结束页主色（对齐设计规格 teal）

/* ==================== 屏幕枚举 ==================== */
typedef enum {
  UI_SCREEN_STANDBY = 0,
  UI_SCREEN_FOCUS,
  UI_SCREEN_PAUSE,
  UI_SCREEN_REST,
  UI_SCREEN_EMOTION,
  UI_SCREEN_SUMMARY,
  UI_SCREEN_LOCKED,
  UI_SCREEN_LOW_BATTERY,
  UI_SCREEN_REST_END,    /* 休息结束提示页 */
  UI_SCREEN_COUNT
} ui_screen_t;

/* ==================== 模式枚举 ==================== */
typedef enum {
  UI_MODE_DEEP_FOCUS,
  UI_MODE_LIGHT_WORK,
  UI_MODE_STUDY,
  UI_MODE_DEEP_REST,
  UI_MODE_LIGHT_REST,
  UI_MODE_STUDY_REST,
} ui_mode_t;

/* ==================== 对外接口 ==================== */
void ui_init(void);
void ui_show_screen(ui_screen_t screen);
ui_screen_t ui_get_current_screen(void);

void ui_set_mode(ui_mode_t mode);
lv_color_t ui_mode_color(ui_mode_t mode);
const char *ui_mode_name(ui_mode_t mode);

void ui_set_timer_text(lv_color_t color, const char *text);
void ui_set_progress(int pct);

typedef void (*ui_emotion_cb_t)(uint8_t index);
void ui_set_emotion_callback(ui_emotion_cb_t cb);


void ui_set_emotion_countdown(uint16_t sec);

void ui_show_total_popup(const char *effTime, const char *ineffTime, const char *restTime);
void ui_show_low_battery(void);
void ui_show_low_battery_pct(uint8_t pct);
void ui_set_lock_clock(const char *timeStr, const char *dateStr);
void ui_set_standby_data(const char *deepTime, const char *lightTime,
                         const char *studyTime, const char *pauseTime);

void ui_show_rest_end(const char *countdownText);
void ui_set_rest_end_countdown(int sec);

#ifdef __cplusplus
}
#endif

#endif /* USE_LVGL */
#endif /* UI_LVGL_PRO_H_ */

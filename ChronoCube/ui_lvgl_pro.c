/**
 * @file ui_lvgl_pro.c
 * ChronoCube LVGL UI — v5.3 新设计实现
 *
 * 原型参考: chronocube-s1-focus-screen/pages/*.html
 *            owner/ui design/效果图/*.png
 * 设计规范: docs/design/UI_implementation_guide_v1.0.md
 *
 * 核心变化 (v5.3):
 *   - 无状态栏 → 全屏极简布局
 *   - 384×384 进度弧 (10px 轨道 + 12px 前景, 圆端)
 *   - Montserrat 36 大字计时
 *   - Catppuccin Mocha 精确配色
 *   - 6 个计时屏共享模板，ui_set_mode() 切换颜色/文字
 *
 * 架构: 8 个预创建屏幕 + widget 缓存, 0ms 切换延迟
 * 字体: cn_font (24×24 中文), Montserrat (12~36 英文)
 * 内存: < 5KB widget 缓存 + ~122KB draw buffer (DMA)
 */
#include "config.h"

#ifdef USE_LVGL
#include "ui_lvgl_pro.h"
#include "font_adapter.h"
#include <string.h>
#include <math.h>

/* ==================== 屏幕专属 widget 缓存 ==================== */

typedef struct {
  lv_obj_t *arc;            /* 384×384 进度弧 */
  lv_obj_t *timer_label;    /* 中心倒计时大字 */
  lv_obj_t *mode_label;     /* 模式名标签 */
  lv_obj_t *start_dot;      /* 弧线起点圆点 (12点方向) */
  lv_obj_t *end_dot;        /* 弧线终点圆点 (随进度移动) */
} timer_widgets_t;

typedef struct {
  lv_obj_t *timer_label;
  lv_obj_t *status_label;
} pause_widgets_t;

typedef struct {
  lv_obj_t *title_label;
  lv_obj_t *deep_name;   lv_obj_t *deep_time;
  lv_obj_t *light_name;  lv_obj_t *light_time;
  lv_obj_t *study_name;  lv_obj_t *study_time;
  lv_obj_t *divider;
  lv_obj_t *pause_name;  lv_obj_t *pause_time;
} standby_widgets_t;

typedef struct {
  lv_obj_t *title_label;
  lv_obj_t *btn[4];
  lv_obj_t *countdown_label;
} emotion_widgets_t;

typedef struct {
  lv_obj_t *title_label;
  lv_obj_t *eff_label;
  lv_obj_t *eff_time;
  lv_obj_t *ineff_label;
  lv_obj_t *ineff_time;
  lv_obj_t *rest_label;
  lv_obj_t *rest_time;
} summary_widgets_t;

typedef struct {
  lv_obj_t *lock_icon_cont; /* 锁图标容器 */
  lv_obj_t *lock_text;
  lv_obj_t *clock_label;
  lv_obj_t *date_label;
} locked_widgets_t;

typedef struct {
  lv_obj_t *battery_icon;
  lv_obj_t *battery_text;
  lv_obj_t *hint_label;
} lowbat_widgets_t;

typedef struct {
  lv_obj_t *title_label;
  lv_obj_t *countdown_label;
  lv_obj_t *hint_label;
} rest_end_widgets_t;

/* ==================== 全局状态 ==================== */
static lv_obj_t *g_screens[UI_SCREEN_COUNT];

static union {
  timer_widgets_t   timer;
  pause_widgets_t   pause;
  standby_widgets_t standby;
  emotion_widgets_t emotion;
  summary_widgets_t summary;
  locked_widgets_t  locked;
  lowbat_widgets_t  lowbat;
  rest_end_widgets_t rest_end;
} g_w[UI_SCREEN_COUNT];

static lv_obj_t     *g_active_screen = NULL;
static ui_screen_t   g_current_id = UI_SCREEN_STANDBY;
static ui_mode_t     g_current_mode = UI_MODE_DEEP_FOCUS;
static ui_emotion_cb_t g_emotion_cb = NULL;

/* ==================== 辅助 ==================== */

static void create_fullscreen_bg(lv_obj_t *scr, lv_color_t color) {
  lv_obj_t *bg = lv_obj_create(scr);
  lv_obj_remove_style_all(bg);
  lv_obj_set_size(bg, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(bg, color, 0);
  lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bg, 0, 0);
  lv_obj_set_style_shadow_width(bg, 0, 0);
  lv_obj_set_style_outline_width(bg, 0, 0);
  lv_obj_set_style_pad_all(bg, 0, 0);
  lv_obj_set_style_radius(bg, 0, 0);
  lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
}

static void set_bg(lv_obj_t *scr, lv_color_t color) {
  lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
  create_fullscreen_bg(scr, color);
}

lv_color_t ui_mode_color(ui_mode_t mode) {
  switch (mode) {
    case UI_MODE_DEEP_FOCUS:
    case UI_MODE_DEEP_REST:   return CP_GREEN;
    case UI_MODE_LIGHT_WORK:
    case UI_MODE_LIGHT_REST:  return CP_PEACH;
    case UI_MODE_STUDY:
    case UI_MODE_STUDY_REST:  return CP_MAUVE;
    default:                  return CP_TEXT;
  }
}

const char *ui_mode_name(ui_mode_t mode) {
  switch (mode) {
    case UI_MODE_DEEP_FOCUS:  return "深度专注";
    case UI_MODE_LIGHT_WORK:  return "轻量事务";
    case UI_MODE_STUDY:       return "学习成长";
    case UI_MODE_DEEP_REST:   return "深度休息";
    case UI_MODE_LIGHT_REST:  return "轻量休息";
    case UI_MODE_STUDY_REST:  return "学习休息";
    default:                  return "";
  }
}

/* ==================== 电池图标构件 ==================== */

static lv_obj_t *create_battery_icon(lv_obj_t *parent, int32_t offset_y) {
  lv_obj_t *cont = lv_obj_create(parent);
  lv_obj_set_size(cont, 64, 32);
  lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);

  /* 电池主体：圆角矩形，3px 红色描边 */
  lv_obj_t *shell = lv_obj_create(cont);
  lv_obj_set_size(shell, 54, 30);
  lv_obj_set_pos(shell, 0, 0);
  lv_obj_set_style_bg_opa(shell, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(shell, 3, 0);
  lv_obj_set_style_border_color(shell, CP_RED, 0);
  lv_obj_set_style_radius(shell, 6, 0);
  lv_obj_set_style_pad_all(shell, 0, 0);

  /* 电池正极（小凸起） */
  lv_obj_t *nipple = lv_obj_create(cont);
  lv_obj_set_size(nipple, 6, 12);
  lv_obj_set_pos(nipple, 54, 9);
  lv_obj_set_style_bg_color(nipple, CP_RED, 0);
  lv_obj_set_style_border_width(nipple, 0, 0);
  lv_obj_set_style_radius(nipple, 2, 0);

  /* 内部填充（约 15% 低电量条） */
  lv_obj_t *fill = lv_obj_create(cont);
  lv_obj_set_size(fill, 8, 18);
  lv_obj_set_pos(fill, 5, 6);
  lv_obj_set_style_bg_color(fill, CP_RED, 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_60, 0);
  lv_obj_set_style_border_width(fill, 0, 0);
  lv_obj_set_style_radius(fill, 2, 0);

  lv_obj_align(cont, LV_ALIGN_CENTER, 0, offset_y);
  return cont;
}

/* ==================== 计时屏模板（FOCUS / REST 共用） ==================== */

/* 弧线圆点参数 */
#define ARC_DOT_R       14      /* 圆点半径 (px)，直径 28 */
#define ARC_RADIUS      186     /* 弧线厚度中心 = 192(外圈) - 12(线宽)/2 = 186 */
#define ARC_CX          240     /* 弧线中心 X (480/2) */
#define ARC_CY          240     /* 弧线中心 Y (480/2) */

static void style_dot(lv_obj_t *dot) {
  lv_obj_set_size(dot, ARC_DOT_R * 2, ARC_DOT_R * 2);
  lv_obj_set_style_radius(dot, ARC_DOT_R, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_style_shadow_width(dot, 0, 0);
  lv_obj_set_style_shadow_opa(dot, LV_OPA_TRANSP, 0);
  lv_obj_set_style_outline_width(dot, 0, 0);
  lv_obj_set_style_outline_opa(dot, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(dot, LV_OPA_COVER, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
}

static void update_arc_dots(timer_widgets_t *w, int pct) {
  if (!w->start_dot || !w->end_dot) return;
  /* 起点圆点：12点方向 (LVGL rotation=270 → 角度270°) */
  float sa = 270.0f * 3.14159f / 180.0f;
  lv_obj_set_pos(w->start_dot,
    ARC_CX + (int)(ARC_RADIUS * cosf(sa)) - ARC_DOT_R,
    ARC_CY + (int)(ARC_RADIUS * sinf(sa)) - ARC_DOT_R);
  /* 终点圆点：随进度移动 */
  float ea = (270.0f + pct * 3.6f) * 3.14159f / 180.0f;
  lv_obj_set_pos(w->end_dot,
    ARC_CX + (int)(ARC_RADIUS * cosf(ea)) - ARC_DOT_R,
    ARC_CY + (int)(ARC_RADIUS * sinf(ea)) - ARC_DOT_R);
}

static void create_timer_screen(lv_obj_t *scr, timer_widgets_t *w) {
  set_bg(scr, CP_MANTLE);

  /* 384×384 进度弧 */
  w->arc = lv_arc_create(scr);
  lv_obj_set_size(w->arc, 384, 384);
  lv_obj_center(w->arc);

  /* 禁用触摸交互 — 进度弧仅做显示，不响应拖动 */
  lv_obj_clear_flag(w->arc, LV_OBJ_FLAG_CLICKABLE);

  /* 背景弧：完整圆, CP_SURFACE1, 10px */
  lv_arc_set_bg_start_angle(w->arc, 0);
  lv_arc_set_bg_end_angle(w->arc, 360);
  lv_obj_set_style_arc_color(w->arc, CP_SURFACE1, LV_PART_MAIN);
  lv_obj_set_style_arc_width(w->arc, 10, LV_PART_MAIN);

  /* 前景弧：0~100%, 模式色, 12px, 圆端 */
  lv_arc_set_value(w->arc, 0);
  lv_arc_set_range(w->arc, 0, 100);
  lv_arc_set_rotation(w->arc, 270);
  lv_obj_set_style_arc_width(w->arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(w->arc, true, LV_PART_INDICATOR);

  /* 隐藏旋钮 */
  lv_obj_set_style_bg_opa(w->arc, LV_OPA_TRANSP, LV_PART_KNOB);

  /* 弧线起点圆点 — 12点方向 */
  w->start_dot = lv_obj_create(scr);
  style_dot(w->start_dot);
  lv_obj_set_style_bg_color(w->start_dot, CP_GREEN, 0);

  /* 弧线终点圆点 — 随进度移动 */
  w->end_dot = lv_obj_create(scr);
  style_dot(w->end_dot);
  lv_obj_set_style_bg_color(w->end_dot, CP_GREEN, 0);

  /* 初始位置：起点在12点，终点重合 */
  update_arc_dots(w, 0);

  /* 中心倒计时 — MM:SS 格式，弧线内居中 */
  w->timer_label = lv_label_create(scr);
  lv_obj_set_width(w->timer_label, 300);
  lv_obj_set_pos(w->timer_label, 90, 170);
  lv_obj_set_style_text_align(w->timer_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->timer_label, "00:00");
  lv_obj_set_style_text_font(w->timer_label, &en_font_96, 0);

  /* 模式名 — 居中对齐 */
  w->mode_label = lv_label_create(scr);
  lv_obj_set_width(w->mode_label, 300);
  lv_obj_set_pos(w->mode_label, 90, 280);
  lv_obj_set_style_text_align(w->mode_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->mode_label, "");
  lv_obj_set_style_text_font(w->mode_label, &cn_font_48, 0);
}

static void apply_timer_mode(timer_widgets_t *w, ui_mode_t mode) {
  lv_color_t c = ui_mode_color(mode);
  const char *name = ui_mode_name(mode);

  lv_obj_set_style_arc_color(w->arc, c, LV_PART_INDICATOR);
  lv_obj_set_style_text_color(w->timer_label, c, 0);
  lv_obj_set_style_text_color(w->mode_label, c, 0);
  lv_label_set_text(w->mode_label, name);
  if (w->start_dot) lv_obj_set_style_bg_color(w->start_dot, c, 0);
  if (w->end_dot) lv_obj_set_style_bg_color(w->end_dot, c, 0);
}

/* ==================== S0 待机仪表板 ==================== */

static void create_s0_standby(lv_obj_t *scr, standby_widgets_t *w) {
  set_bg(scr, CP_MANTLE);

  /* 标题 "今日" */
  w->title_label = lv_label_create(scr);
  lv_obj_set_pos(w->title_label, 0, 80);
  lv_obj_set_width(w->title_label, 480);
  lv_label_set_text(w->title_label, "今日");
  lv_obj_set_style_text_color(w->title_label, CP_SUBTEXT0, 0);
  lv_obj_set_style_text_font(w->title_label, &cn_font, 0);
  lv_obj_set_style_text_align(w->title_label, LV_TEXT_ALIGN_CENTER, 0);

  /* 三行模式数据 */
  const struct {
    lv_obj_t **name; lv_obj_t **time; const char *label; lv_color_t color;
  } rows[] = {
    { &w->deep_name,  &w->deep_time,  "深度专注", CP_GREEN },
    { &w->light_name, &w->light_time, "轻量事务", CP_PEACH },
    { &w->study_name, &w->study_time, "学习成长", CP_MAUVE },
  };

  for (int i = 0; i < 3; i++) {
    lv_obj_t *nm = lv_label_create(scr);
    lv_obj_set_pos(nm, 40, 150 + i * 60);
    lv_label_set_text(nm, rows[i].label);
    lv_obj_set_style_text_color(nm, rows[i].color, 0);
    lv_obj_set_style_text_font(nm, &cn_font, 0);
    *rows[i].name = nm;

    lv_obj_t *tm = lv_label_create(scr);
    lv_obj_set_pos(tm, 280, 148 + i * 60);
    lv_obj_set_width(tm, 160);
    lv_label_set_text(tm, "00:00:00");
    lv_obj_set_style_text_color(tm, rows[i].color, 0);
    lv_obj_set_style_text_font(tm, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(tm, LV_TEXT_ALIGN_RIGHT, 0);
    *rows[i].time = tm;
  }

  /* 分隔线 */
  w->divider = lv_obj_create(scr);
  lv_obj_set_size(w->divider, 400, 1);
  lv_obj_set_pos(w->divider, 40, 340);
  lv_obj_set_style_bg_color(w->divider, CP_SURFACE1, 0);
  lv_obj_set_style_border_width(w->divider, 0, 0);

  /* 休息暂停行 */
  w->pause_name = lv_label_create(scr);
  lv_obj_set_pos(w->pause_name, 40, 365);
  lv_label_set_text(w->pause_name, "休息暂停");
  lv_obj_set_style_text_color(w->pause_name, CP_SUBTEXT1, 0);
  lv_obj_set_style_text_font(w->pause_name, &cn_font, 0);

  w->pause_time = lv_label_create(scr);
  lv_obj_set_pos(w->pause_time, 280, 363);
  lv_obj_set_width(w->pause_time, 160);
  lv_label_set_text(w->pause_time, "00:00:00");
  lv_obj_set_style_text_color(w->pause_time, CP_SUBTEXT1, 0);
  lv_obj_set_style_text_font(w->pause_time, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_align(w->pause_time, LV_TEXT_ALIGN_RIGHT, 0);
}

/* ==================== S4 暂停 ==================== */

static void create_s4_pause(lv_obj_t *scr, pause_widgets_t *w) {
  set_bg(scr, CP_MANTLE);

  /* 计时数字 — 红色, 居中 */
  w->timer_label = lv_label_create(scr);
  lv_obj_set_width(w->timer_label, 300);
  lv_obj_set_pos(w->timer_label, 90, 170);
  lv_obj_set_style_text_align(w->timer_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->timer_label, "00:00");
  lv_obj_set_style_text_color(w->timer_label, CP_RED, 0);
  lv_obj_set_style_text_font(w->timer_label, &en_font_96, 0);

  /* "已暂停" — 灰色, 居中在计时器下方 */
  w->status_label = lv_label_create(scr);
  lv_obj_set_width(w->status_label, 300);
  lv_obj_set_pos(w->status_label, 90, 290);
  lv_obj_set_style_text_align(w->status_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->status_label, "已暂停");
  lv_obj_set_style_text_color(w->status_label, CP_SUBTEXT0, 0);
  lv_obj_set_style_text_font(w->status_label, &cn_font_48, 0);
}

/* ==================== S8 情绪选择 ==================== */

static void emotion_btn_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_CLICKED) return;
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_emotion_cb) g_emotion_cb((uint8_t)idx);
}

static void create_s8_emotion(lv_obj_t *scr, emotion_widgets_t *w) {
  set_bg(scr, CP_MANTLE);

  /* 标题 "感觉如何？" */
  w->title_label = lv_label_create(scr);
  lv_obj_set_pos(w->title_label, 0, 40);
  lv_obj_set_width(w->title_label, 480);
  lv_label_set_text(w->title_label, "感觉如何？");
  lv_obj_set_style_text_color(w->title_label, CP_TEXT, 0);
  lv_obj_set_style_text_align(w->title_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(w->title_label, &cn_font, 0);

  /* 2×2 网格按钮
   * 视觉顺序: 左上=顺畅, 右上=平淡, 左下=卡壳, 右下=耗竭
   * 回调索引: 0=flow, 2=plain, 1=stuck, 3=drained */
  const char *texts[]     = {"顺畅", "平淡", "卡壳", "耗竭"};
  lv_color_t  borders[]   = {CP_GREEN, CP_YELLOW, CP_SUBTEXT1, CP_MAROON};
  const int   emo_index[] = {0, 2, 1, 3};

  int btn_w = 186, btn_h = 140, gap_h = 28, gap_v = 40;
  int total_w = 2 * btn_w + gap_h;
  int start_x = (480 - total_w) / 2;
  int start_y = 98;

  for (int i = 0; i < 4; i++) {
    int row = i / 2, col = i % 2;

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, btn_w, btn_h);
    lv_obj_set_pos(btn, start_x + col * (btn_w + gap_h), start_y + row * (btn_h + gap_v));
    lv_obj_set_style_bg_color(btn, CP_SURFACE0, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_border_color(btn, borders[i], 0);
    lv_obj_set_style_border_width(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, texts[i]);
    lv_obj_center(lbl);
    lv_obj_set_style_text_color(lbl, borders[i], 0);
    lv_obj_set_style_text_font(lbl, &cn_font_48, 0);

    lv_obj_add_event_cb(btn, emotion_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)emo_index[i]);
    w->btn[i] = btn;
  }

  /* 倒计时提示 */
  w->countdown_label = lv_label_create(scr);
  lv_obj_set_pos(w->countdown_label, 0, 440);
  lv_obj_set_width(w->countdown_label, 480);
  lv_label_set_text(w->countdown_label, "10s后自动选择「平淡」");
  lv_obj_set_style_text_color(w->countdown_label, CP_SUBTEXT0, 0);
  lv_obj_set_style_text_align(w->countdown_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(w->countdown_label, &cn_font, 0);
}

/* ==================== S9 总时长 ==================== */

static void create_s9_summary(lv_obj_t *scr, summary_widgets_t *w) {
  set_bg(scr, CP_MANTLE);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  /* 居中卡片 — 480×270, 圆角 20, CP_BASE, 无边框, 不可滚动 */
  lv_obj_t *card = lv_obj_create(scr);
  lv_obj_set_size(card, 480, 270);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, CP_BASE, 0);
  lv_obj_set_style_radius(card, 20, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  /* 高效专注标签 — cn_font, 绿色, 卡片内居中, 顶部 y=36 */
  w->eff_label = lv_label_create(card);
  lv_obj_set_width(w->eff_label, 480);
  lv_obj_align(w->eff_label, LV_ALIGN_TOP_MID, 0, 36);
  lv_label_set_text(w->eff_label, "高效专注");
  lv_obj_set_style_text_color(w->eff_label, CP_GREEN, 0);
  lv_obj_set_style_text_font(w->eff_label, &cn_font, 0);
  lv_obj_set_style_text_align(w->eff_label, LV_TEXT_ALIGN_CENTER, 0);

  /* 高效专注时间 — 96px, 绿色, 标签下方 4px */
  w->eff_time = lv_label_create(card);
  lv_obj_set_width(w->eff_time, 480);
  lv_obj_align_to(w->eff_time, w->eff_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_label_set_text(w->eff_time, "00:00:00");
  lv_obj_set_style_text_color(w->eff_time, CP_GREEN, 0);
  lv_obj_set_style_text_font(w->eff_time, &en_font_96, 0);
  lv_obj_set_style_text_align(w->eff_time, LV_TEXT_ALIGN_CENTER, 0);

  /* 低效专注标签 — cn_font, 灰色, 上方时间底部 +24px */
  w->ineff_label = lv_label_create(card);
  lv_obj_set_width(w->ineff_label, 480);
  lv_obj_align_to(w->ineff_label, w->eff_time, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);
  lv_label_set_text(w->ineff_label, "低效专注");
  lv_obj_set_style_text_color(w->ineff_label, CP_SUBTEXT0, 0);
  lv_obj_set_style_text_font(w->ineff_label, &cn_font, 0);
  lv_obj_set_style_text_align(w->ineff_label, LV_TEXT_ALIGN_CENTER, 0);

  /* 低效专注时间 — 24px, 灰色, 标签下方 2px */
  w->ineff_time = lv_label_create(card);
  lv_obj_set_width(w->ineff_time, 480);
  lv_obj_align_to(w->ineff_time, w->ineff_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
  lv_label_set_text(w->ineff_time, "00:00:00");
  lv_obj_set_style_text_color(w->ineff_time, CP_SUBTEXT0, 0);
  lv_obj_set_style_text_font(w->ineff_time, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_align(w->ineff_time, LV_TEXT_ALIGN_CENTER, 0);

  /* title_label / rest_label / rest_time 不再创建 — 新设计仅 2 行 */
}

/* ==================== 锁定界面 ==================== */

static void create_locked(lv_obj_t *scr, locked_widgets_t *w) {
  set_bg(scr, CP_MANTLE);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  /* "已锁定" — 屏幕水平居中，偏上 */
  w->lock_text = lv_label_create(scr);
  lv_obj_set_width(w->lock_text, 300);
  lv_obj_align(w->lock_text, LV_ALIGN_TOP_MID, 0, 120);
  lv_obj_set_style_text_align(w->lock_text, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->lock_text, "已锁定");
  lv_obj_set_style_text_color(w->lock_text, CP_SUBTEXT0, 0);
  lv_obj_set_style_text_font(w->lock_text, &cn_font, 0);

  /* 时钟 — 屏幕水平居中 */
  w->clock_label = lv_label_create(scr);
  lv_obj_set_width(w->clock_label, 300);
  lv_obj_align(w->clock_label, LV_ALIGN_TOP_MID, 0, 185);
  lv_obj_set_style_text_align(w->clock_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->clock_label, "00:00");
  lv_obj_set_style_text_color(w->clock_label, CP_TEXT, 0);
  lv_obj_set_style_text_font(w->clock_label, &en_font_96, 0);

  /* 日期 — 屏幕水平居中 */
  w->date_label = lv_label_create(scr);
  lv_obj_set_width(w->date_label, 400);
  lv_obj_align(w->date_label, LV_ALIGN_TOP_MID, 0, 310);
  lv_obj_set_style_text_align(w->date_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->date_label, "");
  lv_obj_set_style_text_color(w->date_label, CP_SUBTEXT1, 0);
  lv_obj_set_style_text_font(w->date_label, &cn_font, 0);

  w->lock_icon_cont = NULL;
}

/* ==================== 低电量提醒 ==================== */

static void create_low_battery(lv_obj_t *scr, lowbat_widgets_t *w) {
  set_bg(scr, CP_MANTLE);

  /* --- 电池图标：水平方向，80×80 容器，偏上居中 --- */
  lv_obj_t *cont = lv_obj_create(scr);
  w->battery_icon = cont;
  lv_obj_set_size(cont, 80, 80);
  lv_obj_align(cont, LV_ALIGN_CENTER, 0, -100);
  lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

  /* 电池主体：56×56 圆角矩形，3px 红色描边 */
  lv_obj_t *body = lv_obj_create(cont);
  lv_obj_set_size(body, 56, 56);
  lv_obj_set_pos(body, 8, 12);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(body, 3, 0);
  lv_obj_set_style_border_color(body, CP_RED, 0);
  lv_obj_set_style_radius(body, 6, 0);
  lv_obj_set_style_pad_all(body, 0, 0);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  /* 电池 tip：右侧凸起 8×24 */
  lv_obj_t *tip = lv_obj_create(cont);
  lv_obj_set_size(tip, 8, 24);
  lv_obj_set_pos(tip, 64, 28);
  lv_obj_set_style_bg_color(tip, CP_RED, 0);
  lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(tip, 0, 0);
  lv_obj_set_style_radius(tip, 3, 0);

  /* 低电量填充条 ~15%：8×44，红色 80% 透明度 */
  lv_obj_t *fill = lv_obj_create(cont);
  lv_obj_set_size(fill, 8, 44);
  lv_obj_set_pos(fill, 14, 18);
  lv_obj_set_style_bg_color(fill, CP_RED, 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_80, 0);
  lv_obj_set_style_border_width(fill, 0, 0);
  lv_obj_set_style_radius(fill, 2, 0);

  /* 3 条竖线（空位条纹） */
  for (int i = 0; i < 3; i++) {
    lv_obj_t *line = lv_obj_create(cont);
    lv_obj_set_size(line, 1, 36);
    lv_obj_set_pos(line, 28 + i * 12, 22);
    lv_obj_set_style_bg_color(line, CP_SURFACE1, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
    lv_obj_set_style_border_width(line, 0, 0);
  }

  /* 电量数字 — 96px, 红色, 居中于屏幕 */
  w->battery_text = lv_label_create(scr);
  lv_obj_align(w->battery_text, LV_ALIGN_CENTER, 0, 30);
  lv_label_set_text(w->battery_text, "000");
  lv_obj_set_style_text_color(w->battery_text, CP_RED, 0);
  lv_obj_set_style_text_font(w->battery_text, &en_font_96, 0);
}

/* ==================== 休息结束提示页 ==================== */

static void create_rest_end(lv_obj_t *scr, rest_end_widgets_t *w) {
  create_fullscreen_bg(scr, CP_MANTLE);

  /* 太阳图标 — 用 Montserrat 48 星号代替 */
  lv_obj_t *icon = lv_label_create(scr);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(icon, CP_TEAL, 0);
  lv_label_set_text(icon, "*");
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -100);

  /* "精力已恢复" — 48px, teal */
  w->title_label = lv_label_create(scr);
  lv_obj_set_width(w->title_label, 400);
  lv_obj_set_pos(w->title_label, 40, 170);
  lv_obj_set_style_text_align(w->title_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->title_label, "精力已恢复");
  lv_obj_set_style_text_color(w->title_label, CP_TEAL, 0);
  lv_obj_set_style_text_font(w->title_label, &cn_font_48, 0);

  /* "即将继续专注 · 5s" — 24px */
  w->countdown_label = lv_label_create(scr);
  lv_obj_set_width(w->countdown_label, 400);
  lv_obj_set_pos(w->countdown_label, 40, 240);
  lv_obj_set_style_text_align(w->countdown_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->countdown_label, "即将继续专注 · 5s");
  lv_obj_set_style_text_color(w->countdown_label, CP_TEXT, 0);
  lv_obj_set_style_text_font(w->countdown_label, &cn_font, 0);

  /* "点击屏幕继续 · 5秒后自动返回" — 24px subtext0 */
  w->hint_label = lv_label_create(scr);
  lv_obj_set_width(w->hint_label, 400);
  lv_obj_set_pos(w->hint_label, 40, 320);
  lv_obj_set_style_text_align(w->hint_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(w->hint_label, "点击屏幕继续 · 5秒后自动返回");
  lv_obj_set_style_text_color(w->hint_label, CP_SUBTEXT0, 0);
  lv_obj_set_style_text_font(w->hint_label, &cn_font, 0);
}

/* ==================== ui_init() ==================== */

void ui_init(void) {
  struct {
    ui_screen_t id;
    void (*creator)(lv_obj_t *, void *);
  } defs[] = {
    {UI_SCREEN_STANDBY,    (void (*)(lv_obj_t*,void*))create_s0_standby},
    {UI_SCREEN_FOCUS,      (void (*)(lv_obj_t*,void*))create_timer_screen},
    {UI_SCREEN_PAUSE,      (void (*)(lv_obj_t*,void*))create_s4_pause},
    {UI_SCREEN_REST,       (void (*)(lv_obj_t*,void*))create_timer_screen},
    {UI_SCREEN_EMOTION,    (void (*)(lv_obj_t*,void*))create_s8_emotion},
    {UI_SCREEN_SUMMARY,    (void (*)(lv_obj_t*,void*))create_s9_summary},
    {UI_SCREEN_LOCKED,     (void (*)(lv_obj_t*,void*))create_locked},
    {UI_SCREEN_LOW_BATTERY,(void (*)(lv_obj_t*,void*))create_low_battery},
    {UI_SCREEN_REST_END,   (void (*)(lv_obj_t*,void*))create_rest_end},
  };
  int n = sizeof(defs) / sizeof(defs[0]);

  for (int i = 0; i < n; i++) {
    ui_screen_t id = defs[i].id;
    g_screens[id] = lv_obj_create(NULL);
    memset(&g_w[id], 0, sizeof(g_w[id]));
    defs[i].creator(g_screens[id], &g_w[id]);
  }

  ui_show_screen(UI_SCREEN_STANDBY);
}

/* ==================== 屏幕切换 ==================== */

void ui_show_screen(ui_screen_t id) {
  if (id >= UI_SCREEN_COUNT) return;

  if (!g_screens[id]) return;
  if (g_active_screen == g_screens[id]) return;

  lv_obj_invalidate(g_screens[id]);

  if (g_active_screen)
    lv_screen_load_anim(g_screens[id], LV_SCREEN_LOAD_ANIM_NONE, 0, 0, false);
  else
    lv_screen_load(g_screens[id]);

  g_active_screen = g_screens[id];
  g_current_id = id;
}

ui_screen_t ui_get_current_screen(void) {
  return g_current_id;
}

/* ==================== 模式切换 ==================== */

void ui_set_mode(ui_mode_t mode) {
  g_current_mode = mode;

  if (g_current_id == UI_SCREEN_FOCUS || g_current_id == UI_SCREEN_REST) {
    timer_widgets_t *w = &g_w[g_current_id].timer;
    apply_timer_mode(w, mode);
  }
}

/* ==================== 动态更新 API ==================== */

void ui_set_timer_text(lv_color_t color, const char *text) {
  lv_obj_t *label = NULL;

  switch (g_current_id) {
    case UI_SCREEN_FOCUS:
    case UI_SCREEN_REST:
      label = g_w[g_current_id].timer.timer_label;
      break;
    case UI_SCREEN_PAUSE:
      label = g_w[UI_SCREEN_PAUSE].pause.timer_label;
      break;
    default:
      return;
  }

  if (!label) return;
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, color, 0);
}

void ui_set_progress(int pct) {
  if (g_current_id != UI_SCREEN_FOCUS && g_current_id != UI_SCREEN_REST) return;
  timer_widgets_t *w = &g_w[g_current_id].timer;
  if (!w->arc) return;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  lv_arc_set_value(w->arc, pct);
  update_arc_dots(w, pct);
}

/* ==================== S9 总时长 ==================== */

void ui_show_total_popup(const char *effTime, const char *ineffTime, const char *restTime) {
  summary_widgets_t *w = &g_w[UI_SCREEN_SUMMARY].summary;
  if (effTime && w->eff_time)     lv_label_set_text(w->eff_time, effTime);
  if (ineffTime && w->ineff_time) lv_label_set_text(w->ineff_time, ineffTime);
  if (restTime && w->rest_time)   lv_label_set_text(w->rest_time, restTime);
  ui_show_screen(UI_SCREEN_SUMMARY);
}

/* ==================== 低电量 ==================== */

void ui_show_low_battery(void) {
  ui_show_screen(UI_SCREEN_LOW_BATTERY);
}

void ui_show_low_battery_pct(uint8_t pct) {
  lowbat_widgets_t *w = &g_w[UI_SCREEN_LOW_BATTERY].lowbat;
  if (w->battery_text) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%03d", pct);
    lv_label_set_text(w->battery_text, buf);
  }
  ui_show_screen(UI_SCREEN_LOW_BATTERY);
}

void ui_set_lock_clock(const char *timeStr, const char *dateStr) {
  locked_widgets_t *w = &g_w[UI_SCREEN_LOCKED].locked;
  if (timeStr && w->clock_label) lv_label_set_text(w->clock_label, timeStr);
  if (dateStr && w->date_label)  lv_label_set_text(w->date_label, dateStr);
}

/* ==================== S0 待机数据 ==================== */

void ui_set_standby_data(const char *deepTime, const char *lightTime,
                         const char *studyTime, const char *pauseTime) {
  standby_widgets_t *w = &g_w[UI_SCREEN_STANDBY].standby;

  if (deepTime)  lv_label_set_text(w->deep_time,  deepTime);
  if (lightTime) lv_label_set_text(w->light_time, lightTime);
  if (studyTime) lv_label_set_text(w->study_time, studyTime);
  if (pauseTime) lv_label_set_text(w->pause_time, pauseTime);
}

/* ==================== 情绪 API ==================== */

void ui_set_emotion_countdown(uint16_t sec) {
  emotion_widgets_t *w = &g_w[UI_SCREEN_EMOTION].emotion;
  if (!w->countdown_label) return;
  char buf[32];
  snprintf(buf, sizeof(buf), "%02us后自动选择「平淡」", sec);
  lv_label_set_text(w->countdown_label, buf);
}

void ui_set_emotion_callback(ui_emotion_cb_t cb) {
  g_emotion_cb = cb;
}

void ui_show_rest_end(const char *countdownText) {
  rest_end_widgets_t *w = &g_w[UI_SCREEN_REST_END].rest_end;
  if (countdownText && w->countdown_label)
    lv_label_set_text(w->countdown_label, countdownText);
  /* 注意：不在此调用 ui_show_screen，由调用方在创建 widgets 后再调此函数 */
}

void ui_set_rest_end_countdown(int sec) {
  rest_end_widgets_t *w = &g_w[UI_SCREEN_REST_END].rest_end;
  if (!w->countdown_label) return;
  char buf[48];
  if (sec > 0) {
    snprintf(buf, sizeof(buf), "即将继续专注 · %ds", sec);
  } else {
    snprintf(buf, sizeof(buf), "即将继续专注 · 0s");
  }
  lv_label_set_text(w->countdown_label, buf);
}

#endif /* USE_LVGL */

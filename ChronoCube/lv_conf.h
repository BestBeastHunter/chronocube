/**
 * @file lv_conf.h
 * LVGL v9.5 Configuration for ESP32-C6 ChronoCube
 * 480×480 CO5300 AMOLED, 16bit RGB565, QSPI
 * No PSRAM, max ~150KB allocated for LVGL
 *
 * This file is placed in the Arduino sketch directory.
 * Arduino library LVGL auto-discovers lv_conf.h via:
 *   #if __has_include("lv_conf.h")
 *   #include "lv_conf.h"
 *   #endif
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*
 * NOTE: Do NOT #include <Arduino.h> here!
 * LVGL's lv_types.h wraps all includes in extern "C", which causes
 * "template with C linkage" errors when C++ STL headers are pulled in
 * via Arduino.h → ESP32 framework headers.
 *
 * .ino files auto-include Arduino.h. LVGL's tick module includes
 * Arduino.h via LV_TICK_CUSTOM_INCLUDE in C context where it's safe.
 */

/* =================== Color =================== */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1   /* lv_refr.c auto swap in one place only -- no double-swap risk */

/* =================== Memory =================== */
#define LV_MEM_SIZE (96 * 1024)           /* 96KB for LVGL heap — 8 pre-created screens + CJK font rendering */

/* =================== Rendering =================== */
#define LV_DPI_DEF 160
#define LV_DISP_DEF_REFR_PERIOD 30        /* 30ms refresh = ~33fps */

/* =================== Tick =================== */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* =================== OS / Tasks =================== */
#define LV_USE_OS 0           /* Bare metal (no FreeRTOS wrapper) */

/* =================== Widgets =================== */
/* P2精简: 仅保留 UI 实际使用的 widget（label/btn/arc/line/image），关闭 12 个无用项省 Flash */
#define LV_USE_BUTTON     1
#define LV_USE_LABEL      1
#define LV_USE_IMAGE      1
#define LV_USE_ARC        1
#define LV_USE_LINE       1
#define LV_USE_BAR        0
#define LV_USE_SLIDER     0
#define LV_USE_ROLLER     0
#define LV_USE_DROPDOWN   0
#define LV_USE_ANIMIMG    0
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_CHECKBOX   0
#define LV_USE_KEYBOARD   0
#define LV_USE_LED        0
#define LV_USE_LIST       0
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_SPINNER    0
#define LV_USE_SWITCH     0
#define LV_USE_TABLE      0
#define LV_USE_TABVIEW    0
#define LV_USE_TEXTAREA   1   /* spinbox 依赖 textarea */
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_SPAN       0

/* =================== Log =================== */
#define LV_USE_LOG 0
#if LV_USE_LOG
  #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#endif

/* =================== Fonts =================== */
/* P2精简: 仅保留 UI 实际使用的 7 个字号 (12/14/16/20/24/36/48) */
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_18  0
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_28  0
#define LV_FONT_MONTSERRAT_36  1
#define LV_FONT_MONTSERRAT_48  1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

/* =================== Misc =================== */
#define LV_USE_ANIMATION 1
#define LV_USE_SNAPSHOT  0     /* P2: screenshot 已禁用（USB-CDC 坏），关掉省内存 */
#define LV_USE_SDL       0

/* =================== Layouts =================== */
#define LV_USE_FLEX      1     /* Flex layout */
#define LV_USE_GRID      0     /* Grid layout not used */

/* =================== File System =================== */
#define LV_USE_FS_STDIO  0     /* Desktop stdio FS — NOT for ESP32 */

/* =================== Debug =================== */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_REFR_DEBUG   0

/* =================== Draw =================== */
/* v9: mask is built into the draw subsystem */

#endif /*LV_CONF_H*/

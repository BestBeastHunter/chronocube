#ifndef LVGL_BRIDGE_H
#define LVGL_BRIDGE_H

#ifdef USE_LVGL

#include "config.h"
#include "ui_lvgl_pro.h"

/* LVGL 桥接层：连接 ChronoCube 状态机与 LVGL UI */

/* 状态 → 颜色映射（工作/暂停/情绪=GREEN/PEACH/MAUVE/RED） */
lv_color_t colorForState(SystemState state);

/* 根据当前 SystemState 切换 LVGL 屏幕 + 设置模式颜色 */
void lvglBridge_showScreenForState(SystemState state);

/* 更新 LVGL 计时屏数据（每秒调用） */
void lvglBridge_updateData(unsigned long remainSec, int pct, SystemState state);

/* 更新 S0 待机仪表板数据（深度/轻量/学习 + 暂停） */
void lvglBridge_updateStandby(unsigned long deepSec,
                               unsigned long lightSec,
                               unsigned long studySec,
                               unsigned long pauseSec);

/* 锁定控制 */
void lvglBridge_setLocked(bool locked);

/* 显示 S9 总时长弹窗 */
void lvglBridge_showSummaryPopup(unsigned long effectiveSec,
                                  unsigned long ineffectiveSec,
                                  unsigned long restSec);

/* 格式化锁屏日期字符串（无 RTC 时返回占位） */
void lvglBridge_getLockDateStr(char *buf, size_t len);

#endif /* USE_LVGL */
#endif /* LVGL_BRIDGE_H */

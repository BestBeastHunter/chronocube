#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include "config.h"

#ifdef USE_LVGL

#include <Arduino.h>

// 初始化截屏模块（注册串口命令处理器）
void screenshotInit();

// 在 loop() 中周期性调用，检查串口截屏命令
void screenshotTick();

// 由 debugConsole 转发 "shot" 命令触发（flush-intercept，OOM 安全）
void screenshotTrigger();

#endif // USE_LVGL

#endif // SCREENSHOT_H

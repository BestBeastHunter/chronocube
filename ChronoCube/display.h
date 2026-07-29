#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <stdint.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include "config.h"

// 颜色（RGB565）
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_GRAY    0x7BEF
#define COLOR_DARK    0x2945
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_ORANGE  0xFD20

// AMOLED 显示屏（基于 SH8601/CO5300 QSPI 面板，不依赖 LVGL）
// 文档 4.3
class Display {
public:
  bool begin();                       // 初始化 QSPI 面板
  void setOn(bool on);                // 开关显示（控制电源+BL）
  bool isOn();
  // L2 深度休眠唤醒后重新初始化 LCD（PMU 复位序列 + panel_init + 恢复亮度）
  // 仅在 L2→L0 唤醒时由 PowerManager 唤醒回调调用；屏供电已由 PowerManager 恢复
  void reinitAfterWake();
  // 全局亮度目标（0-100）：机内控制面板 / MQTT set_brightness / 默认配置共用唯一来源
  void setBrightness(uint8_t pct);
  uint8_t getBrightness();            // 返回全局亮度目标（熄屏时不为 0）
  // 息屏/亮屏：仅切换面板亮度（0x51 指令）。息屏=亮度0（全黑，GRAM保留）；
  // 亮屏=恢复全局亮度，画面瞬间恢复，无需 LVGL 重绘
  void setBrightnessEnabled(bool en);

  // 基础绘图（原语）
  void clear(uint16_t color = COLOR_BLACK);
  void drawPixel(int16_t x, int16_t y, uint16_t color);
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawText(int16_t x, int16_t y, const char *s, uint16_t color, uint8_t size = 2);
  void drawTextCenter(int16_t y, const char *s, uint16_t color, uint8_t size = 2);

  // 中文显示（UTF-8 字符串，24x24 点阵，SD 卡字库）
  void drawChinese(int16_t x, int16_t y, const char *s, uint16_t color, uint8_t size = 1);
  void drawChineseCenter(int16_t y, const char *s, uint16_t color, uint8_t size = 1);
  int16_t chineseWidth(const char *s, uint8_t size = 1);

  // 屏幕旋转（0/90/180/270 度），硬件 MADCTL 实现
  void setRotation(uint16_t deg);
  uint16_t getRotation() { return rotation; }

  // 调试：直接发送 MADCTL 值测试方向（校准用）
  void testMADCTL(uint8_t madctl);

  // 获取旋转索引对应的 MADCTL 寄存器值（供 debug 控制台 rot 命令使用）
  static uint8_t getMADCTL(uint8_t idx);

  // 坏点测试：填充整个屏幕为纯色
  void deadPixelTest(uint16_t color);

  // 圆弧绘制（进度环）
  // cx,cy: 圆心 | r: 半径 | startAngle/endAngle: 0-360（0=12点方向，顺时针）
  // color: RGB565 | thickness: 线宽(px)
  void drawArc(int16_t cx, int16_t cy, int16_t r,
               float startAngle, float endAngle,
               uint16_t color, uint8_t thickness = 4);

  // LVGL 集成用：返回 LCD panel handle
  esp_lcd_panel_handle_t getPanelHandle() const;

  // 截图用：返回 LCD IO handle（直接读写 QSPI）
  esp_lcd_panel_io_handle_t getIOHandle() const;

  // 批量绘制：先关显示避免撕裂，画完再开
  void beginDraw();
  void endDraw();

  // 时间字符串助手
  static void formatHMS(unsigned long sec, char *out, size_t outlen);

private:
  bool on;
  uint8_t brightness;        // 当前实际写入面板的值（亮屏=全局亮度，熄屏=0）
  uint8_t userBrightness;    // 全局亮度目标（机内面板/MQTT/默认共用）
  uint16_t rotation;      // 0/90/180/270
  bool drawing;           // 批量绘制中（beginDraw/endDraw 配对）

  // 静态 DMA 缓冲区，避免每次 fillRect/clear 都 malloc/free
  static uint16_t s_dmaBuf[LCD_H_RES * 20];  // 480*20=9600 像素=19200 字节

  // 字模绘制辅助（英文12x24 + 中文24x24）
  void drawENGlyph(int16_t x, int16_t y, const uint8_t *glyph, uint16_t color, uint8_t size);
  void drawCNGlyph(int16_t x, int16_t y, const uint8_t *glyph, uint16_t color, uint8_t size);
};

extern Display display;

#endif

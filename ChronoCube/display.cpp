#include "display.h"
#include "config.h"
#include "pmu.h"
#include "font_loader.h"
#include "spi_bus_lock.h"
#ifdef USE_LVGL
#include "lvgl.h"
#include "lv_port_disp.h"
#endif

#include <math.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include "src/esp_lcd_sh8601.h"   // SH8601 panel driver（与 CO5300 兼容）

Display display;

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;

// 静态 DMA 缓冲区定义
uint16_t Display::s_dmaBuf[LCD_H_RES * 20];

static inline uint16_t swap565(uint16_t c) {
  return (c >> 8) | (c << 8);
}

// 屏幕旋转对应 MADCTL 值（0x36 寄存器）
// SH8601/CO5300 MADCTL 位：
//   BIT7: MY  行地址顺序
//   BIT6: MX  列地址顺序
//   BIT5: MV  行列交换
//   BIT4: ML  垂直刷新顺序
//   BIT3: RGB RGB/BGR 顺序
//   BIT2: MH  水平刷新顺序
// MADCTL 值含义（实测确认）：
//   0x30: MV+ML → 0° 直立
//   0x90: MY+ML → 90° 顺时针
//   0x60: MX+MV → 180° 倒立
//   0xC0: MY+MX → 270° 逆时针
static const uint8_t madctl_table[4] = {
  0x30,   // 0°   直立（MV+ML）
  0x90,   // 90°  顺时针（MY+ML）
  0x60,   // 180° 倒立（MX+MV）
  0xC0,   // 270° 逆时针（MY+MX）
};
static uint8_t degToIdx(uint16_t deg) {
  switch (deg) {
    case 90:  return 1;
    case 180: return 2;
    case 270: return 3;
    default:  return 0;
  }
}

void Display::setRotation(uint16_t deg) {
  uint8_t idx = degToIdx(deg);
  uint8_t madctl = madctl_table[idx];
  Serial.printf("[LCD] setRotation %u° (MADCTL=0x%02X idx=%u)\n", deg, madctl, idx);
  // QSPI 32bit 命令格式：byte0=0x00, byte1=寄存器地址(0x36), byte3=0x02(LCD_OPCODE_WRITE_CMD)
  // MADCTL 值必须作为独立参数数据发送，而非嵌入命令字（否则被当作地址）
  uint32_t cmd = (0x36 << 8) | (0x02UL << 24);
  spi2_lock();
  esp_lcd_panel_io_tx_param(io_handle, (int)cmd, &madctl, 1);
  spi2_unlock();
  rotation = deg;
  lv_obj_invalidate(lv_screen_active());  // 触发 LVGL 全屏重绘以匹配新旋转方向
}

void Display::testMADCTL(uint8_t madctl) {
  uint32_t cmd = (0x36 << 8) | (0x02UL << 24);
  spi2_lock();
  esp_lcd_panel_io_tx_param(io_handle, (int)cmd, &madctl, 1);
  spi2_unlock();
}

uint8_t Display::getMADCTL(uint8_t idx) {
  return (idx < 4) ? madctl_table[idx] : 0x30;
}

void Display::deadPixelTest(uint16_t color) {
  fillRect(0, 0, LCD_H_RES, LCD_V_RES, color);
}
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
  { 0x11, (uint8_t[]){ 0x00 }, 0, 600 },
  { 0xFE, (uint8_t[]){ 0x20 }, 1, 0 },
  { 0x19, (uint8_t[]){ 0x10 }, 1, 0 },
  { 0x1C, (uint8_t[]){ 0xA0 }, 1, 0 },
  { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
  { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },  // RGB565
  { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0x36, (uint8_t[]){ 0x30 }, 1, 0 },  // MADCTL: 与微雪官方一致
  { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },
  { 0x51, (uint8_t[]){ 0xFF }, 1, 0 },  // 亮度 255
  { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
  { 0x2A, (uint8_t[]){ 0x00, 0x00, 0x01, 0xDF }, 4, 0 },  // 0..479
  { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xDF }, 4, 0 },  // 0..479
  { 0x29, (uint8_t[]){ 0x00 }, 0, 100 },
};

bool Display::begin() {
  // 1) AXP2101 ALDO3 先开电（让 PMU 稳定），后面 new panel 后再做复位
  powerManager.enableLcdPower(true);
  delay(10);

  // 2) QSPI 总线初始化
  spi_bus_config_t buscfg = {};
  buscfg.sclk_io_num = PIN_LCD_QSPI_SCK;
  buscfg.data0_io_num= PIN_LCD_QSPI_D0;
  buscfg.data1_io_num= PIN_LCD_QSPI_D1;
  buscfg.data2_io_num= PIN_LCD_QSPI_D2;
  buscfg.data3_io_num= PIN_LCD_QSPI_D3;
  buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * LCD_BITS_PER_PIXEL / 8;
  if (spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
    Serial.println("[LCD] spi_bus_initialize failed");
    return false;
  }

  // 3) LCD IO 绑定到 SPI2
  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = PIN_LCD_CS;
  io_config.dc_gpio_num = -1;
  io_config.spi_mode    = 0;
  io_config.pclk_hz     = LCD_PIXEL_CLOCK_HZ;
  io_config.trans_queue_depth = 2;  /* =2: matches official Waveshare example */
  io_config.lcd_cmd_bits = 32;   // SH8601 命令为 32 位
  io_config.lcd_param_bits = 8;
  io_config.flags.quad_mode = true;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle) != ESP_OK) {
    Serial.println("[LCD] esp_lcd_new_panel_io_spi failed");
    return false;
  }

  // 4) 创建 SH8601 面板
  sh8601_vendor_config_t vendor_config = {};
  vendor_config.init_cmds = lcd_init_cmds;
  vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
  vendor_config.flags.use_qspi_interface = 1;

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = -1;             // 无独立 RST，通过 PMU 复位
  panel_config.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;  // 与微雪官方一致
  panel_config.bits_per_pixel = LCD_BITS_PER_PIXEL;
  panel_config.vendor_config  = &vendor_config;

  if (esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle) != ESP_OK) {
    Serial.println("[LCD] esp_lcd_new_panel_sh8601 failed");
    return false;
  }

  // 5) PMU 做 LCD 复位（开→关→开，与微雪官方一致）
  powerManager.enableLcdPower(true);
  delay(100);
  powerManager.enableLcdPower(false);
  delay(100);
  powerManager.enableLcdPower(true);
  delay(100);

  // 6) 初始化面板
  if (esp_lcd_panel_init(panel_handle) != ESP_OK) {
    Serial.println("[LCD] panel init failed");
    return false;
  }
  if (esp_lcd_panel_disp_on_off(panel_handle, true) != ESP_OK) {
    Serial.println("[LCD] disp_on_off failed");
    return false;
  }

  on = true;
  brightness = 100;
  userBrightness = 100;     // 全局亮度目标默认满亮，与 brightness 一致
  rotation = 0;
  drawing = false;

  // 硬件自检：先填充全白确认 QSPI 通路
  clear(0xFFFF);
  delay(500);
  // 再填充全黑
  clear(COLOR_BLACK);

  Serial.println("[LCD] SH8601 ready (self-test: white→black)");
  return true;
}

void Display::setOn(bool en) {
  if (!panel_handle) return;
  if (en == on) return;
  esp_lcd_panel_disp_on_off(panel_handle, en);
  on = en;
}
bool Display::isOn() { return on; }

void Display::reinitAfterWake() {
  // L2 深度休眠唤醒时由 PowerManager 唤醒回调调用：屏供电已由 setPowerLevel(L0) 的
  // enableLcdPower(true) 恢复，但 SH8601 断电后需重新 PMU 复位序列 + panel_init 才能显示。
  // 复用既有 panel_handle，不 esp_lcd_panel_del+new（否则 LVGL flush 回调指向旧 handle 失效）。
  if (!panel_handle) return;

#ifdef USE_LVGL
  // 暂停 LVGL flush，防止 panel_init 期间 DMA 传输抢 SPI2 导致首帧撕裂
  lvglSetScreenEnabled(false);
#endif

  powerManager.enableLcdPower(true);
  delay(50);
  powerManager.enableLcdPower(false);   // PMU 复位（SH8601 无独立 RST 引脚）
  delay(50);
  powerManager.enableLcdPower(true);
  delay(50);
  spi2_lock();                          // 唤醒时 LVGL 可能仍在刷屏，保护 QSPI 命令
  if (esp_lcd_panel_init(panel_handle) != ESP_OK) {
    spi2_unlock();
#ifdef USE_LVGL
    lvglSetScreenEnabled(true);         // 失败也恢复 LVGL，避免永久黑屏
#endif
    Serial.println("[LCD] reinitAfterWake: panel_init failed");
    return;
  }
  spi2_unlock();
  on = true;                            // panel_init 后显示默认开启
  setBrightnessEnabled(true);          // 恢复全局亮度，画面瞬间恢复（display.h:35）

#ifdef USE_LVGL
  lvglSetScreenEnabled(true);          // 恢复 LVGL 渲染
#endif
  Serial.println("[LCD] reinitAfterWake done");
}

void Display::setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  userBrightness = pct;                 // 写入全局亮度目标（机内面板/MQTT/默认共用）
  if (!panel_handle) return;
  if (brightness == 0) return;          // 当前处于熄屏状态：仅记录，亮屏时由 setBrightnessEnabled(true) 恢复
  uint8_t val = (pct * 255) / 100;
  brightness = pct;
  uint32_t cmd = (0x51 << 8) | (0x02UL << 24);
  spi2_lock();
  esp_lcd_panel_io_tx_param(io_handle, (int)cmd, &val, 1);
  spi2_unlock();
}

uint8_t Display::getBrightness() { return userBrightness; }   // 返回全局亮度目标，熄屏时也不为 0

/* 息屏/亮屏：仅切换面板亮度（0x51 指令），不触达 GRAM。
 * 熄屏=亮度0（画面全黑，GRAM 仍保留最后一帧）；
 * 亮屏=恢复全局亮度，画面瞬间恢复，无需 LVGL 全屏重绘。 */
void Display::setBrightnessEnabled(bool en) {
  if (!panel_handle) return;
  uint8_t target = en ? userBrightness : 0;
  uint8_t val = (target * 255) / 100;
  brightness = target;
  uint32_t cmd = (0x51 << 8) | (0x02UL << 24);
  spi2_lock();
  esp_lcd_panel_io_tx_param(io_handle, (int)cmd, &val, 1);
  spi2_unlock();
}

void Display::clear(uint16_t color) {
  if (!panel_handle) return;
  uint16_t c = swap565(color);
  const int blockH = 20;
  size_t blockPixels = (size_t)LCD_H_RES * blockH;
  // 使用静态缓冲区，无 malloc/free 开销
  for (size_t i = 0; i < blockPixels; i++) s_dmaBuf[i] = c;
  int drawn = 0;
  spi2_lock();  /* P0: SPI2 总线互斥 — 保护整个 clear 操作 */
  while (drawn < LCD_V_RES) {
    int curH = blockH;
    if (drawn + curH > LCD_V_RES) curH = LCD_V_RES - drawn;
    if (curH % 2 != 0) curH++;
    if (drawn + curH > LCD_V_RES) curH = LCD_V_RES - drawn;
    esp_lcd_panel_draw_bitmap(panel_handle, 0, drawn, LCD_H_RES, drawn + curH, s_dmaBuf);
    drawn += curH;
  }
  spi2_unlock();
}

void Display::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= LCD_H_RES || y >= LCD_V_RES) return;
  uint16_t c = swap565(color);
  uint16_t buf[4];  // 2x2 块，保证偶数对齐
  for (int i = 0; i < 4; i++) buf[i] = c;
  // v5.3.11: 移除像素偏移（与 LVGL 冲突）
  spi2_lock();
  esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 2, y + 2, buf);
  spi2_unlock();
}

void Display::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LCD_H_RES) w = LCD_H_RES - x;
  if (y + h > LCD_V_RES) h = LCD_V_RES - y;
  if (w <= 0 || h <= 0) return;

  // v5.3.11: 移除像素偏移（与 LVGL 冲突）
  int sx = x;
  int sy = y;
  int sw = w;
  int sh = h;

  // P2-1: 使用静态 DMA 缓冲区，避免每次 alloc/free
  uint16_t c = swap565(color);
  int blockH = 20;
  if (blockH > sh) blockH = sh;
  size_t blockPixels = (size_t)sw * blockH;
  // 静态缓冲区容量检查（安全边际）
  if (blockPixels > (LCD_H_RES * 20)) return;

  for (size_t i = 0; i < blockPixels; i++) s_dmaBuf[i] = c;
  int drawn = 0;
  spi2_lock();  /* P0: SPI2 总线互斥 — 保护整个 fillRect 操作 */
  while (drawn < sh) {
    int curH = blockH;
    if (drawn + curH > sh) curH = sh - drawn;
    if (curH > 1 && (curH & 1)) curH--;  /* QSPI 需偶数行高 */
    if (curH == 0) break;  /* 防御：防止最后 1 行循环无法退出 */
    esp_lcd_panel_draw_bitmap(panel_handle, sx, sy + drawn, sx + sw, sy + drawn + curH, s_dmaBuf);
    drawn += curH;
  }
  spi2_unlock();
}

void Display::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  fillRect(x, y, w, 1, color);
  fillRect(x, y+h-1, w, 1, color);
  fillRect(x, y, 1, h, color);
  fillRect(x+w-1, y, 1, h, color);
}

// ========== 英文绘制（12×24 SD 卡字库）==========

// 绘制单个 12×24 英文字模
// glyph: 48 字节数据（24行 × 2字节/行，每行16位低12位有效，MSB-first）
void Display::drawENGlyph(int16_t x, int16_t y, const uint8_t *glyph, uint16_t color, uint8_t size) {
  bool fromSD = fontLoader.isInENCache(glyph);
  for (int row = 0; row < EN_FONT_H; row++) {
    for (int col = 0; col < EN_FONT_W; col++) {
      int byteIdx = row * 2 + col / 8;
      int bitIdx = 7 - (col % 8);
      uint8_t bit;
      if (fromSD) {
        bit = glyph[byteIdx] & (1 << bitIdx);
      } else {
        bit = pgm_read_byte(&glyph[byteIdx]) & (1 << bitIdx);
      }
      if (bit) {
        if (size == 1) {
          drawPixel(x + col, y + row, color);
        } else {
          fillRect(x + col * size, y + row * size, size, size, color);
        }
      }
    }
  }
}

void Display::drawText(int16_t x, int16_t y, const char *s, uint16_t color, uint8_t size) {
  if (!s) return;
  int16_t cx = x;
  while (*s) {
    char c = *s++;
    const uint8_t *glyph = nullptr;
    if (fontLoader.isENReady()) {
      glyph = fontLoader.getENGlyph((uint8_t)c);
    }
    if (glyph) {
      drawENGlyph(cx, y, glyph, color, size);
      cx += EN_FONT_W * size + size;   // 字宽 + 1px 间距
    } else {
      // 找不到的字符画一个窄方块
      fillRect(cx, y, 8 * size, EN_FONT_H * size, color);
      cx += 8 * size + size;
    }
  }
}

void Display::drawTextCenter(int16_t y, const char *s, uint16_t color, uint8_t size) {
  if (!s) return;
  int len = 0;
  const char *p = s; while (*p++) len++;
  // 每个字符宽度 = 12 * size + size 间距
  int w = len * (EN_FONT_W * size + size);
  drawText((LCD_H_RES - w) / 2, y, s, color, size);
}

// ========== 中文绘制（24×24 SD 卡字库）==========

// UTF-8 字节长度判断
static int utf8ByteLen(uint8_t b) {
  if ((b & 0x80) == 0) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 1;
}

// 绘制单个 24×24 中文字模
// glyph: 72 字节点阵数据（24×24÷8=72），每行 3 字节
void Display::drawCNGlyph(int16_t x, int16_t y, const uint8_t *glyph, uint16_t color, uint8_t size) {
  bool fromSD = fontLoader.isInCNCache(glyph);
  for (int row = 0; row < CN_FONT_H; row++) {
    for (int col = 0; col < CN_FONT_W; col++) {
      int byteIdx = row * 3 + col / 8;
      int bitIdx = 7 - (col % 8);
      uint8_t bit;
      if (fromSD) {
        bit = glyph[byteIdx] & (1 << bitIdx);
      } else {
        bit = pgm_read_byte(&glyph[byteIdx]) & (1 << bitIdx);
      }
      if (bit) {
        if (size == 1) {
          drawPixel(x + col, y + row, color);
        } else {
          fillRect(x + col * size, y + row * size, size, size, color);
        }
      }
    }
  }
}

int16_t Display::chineseWidth(const char *s, uint8_t size) {
  if (!s) return 0;
  int16_t w = 0;
  while (*s) {
    int len = utf8ByteLen((uint8_t)*s);
    w += CN_FONT_W * size + 2 * size;
    s += len;
  }
  return w;
}

void Display::drawChinese(int16_t x, int16_t y, const char *s, uint16_t color, uint8_t size) {
  if (!s) return;
  int16_t cx = x;

  while (*s) {
    int len = utf8ByteLen((uint8_t)*s);
    const uint8_t *glyph = nullptr;

    if (fontLoader.isCNReady()) {
      glyph = fontLoader.getCNGlyph((const uint8_t *)s, len);
    }

    if (glyph) {
      drawCNGlyph(cx, y, glyph, color, size);
      cx += CN_FONT_W * size + 2 * size;
    } else {
      // 找不到的字符画方框
      fillRect(cx, y, 12 * size, CN_FONT_H * size, color);
      cx += 12 * size + 2 * size;
    }

    s += len;
  }
}

void Display::drawChineseCenter(int16_t y, const char *s, uint16_t color, uint8_t size) {
  int16_t w = chineseWidth(s, size);
  drawChinese((LCD_H_RES - w) / 2, y, s, color, size);
}

void Display::drawArc(int16_t cx, int16_t cy, int16_t r,
                      float startAngle, float endAngle,
                      uint16_t color, uint8_t thickness) {
  if (r <= 0 || thickness == 0) return;
  if (endAngle - startAngle < 0.5f) return;

  int steps = (int)((endAngle - startAngle) * 2.5f);
  if (steps < 8) steps = 8;
  if (steps > 720) steps = 720;

  int16_t prevX = 0, prevY = 0;
  bool first = true;

  for (int i = 0; i <= steps; i++) {
    float angle = startAngle + (endAngle - startAngle) * i / (float)steps;
    float rad = angle * PI / 180.0f;
    int16_t px = cx + (int16_t)(r * sin(rad));
    int16_t py = cy - (int16_t)(r * cos(rad));

    if (!first) {
      int16_t dx = px - prevX;
      int16_t dy = py - prevY;
      int dist = (int)sqrt((float)(dx * dx + dy * dy));
      if (dist > 0 && dist < 100) {
        for (int t = 0; t <= dist; t++) {
          int16_t tx = prevX + dx * t / dist;
          int16_t ty = prevY + dy * t / dist;
          if (thickness == 1) {
            drawPixel(tx, ty, color);
          } else {
            fillRect(tx - (int16_t)(thickness / 2), ty - (int16_t)(thickness / 2),
                     thickness, thickness, color);
          }
        }
      }
    }
    prevX = px;
    prevY = py;
    first = false;
  }
}

void Display::formatHMS(unsigned long sec, char *out, size_t outlen) {
  int h = sec / 3600;
  int m = (sec % 3600) / 60;
  int s = sec % 60;
  snprintf(out, outlen, "%02d:%02d:%02d", h, m, s);
}

// ========== 批量绘制（防撕裂）==========
// AMOLED 像素自保持：关扫描期间写 GRAM 不会产生撕裂
void Display::beginDraw() {
  if (!panel_handle || drawing) return;
  drawing = true;
  esp_lcd_panel_disp_on_off(panel_handle, false);
}

void Display::endDraw() {
  if (!panel_handle || !drawing) return;
  esp_lcd_panel_disp_on_off(panel_handle, true);
  on = true;  // panel 已重新开启
  drawing = false;
}

// LVGL 集成：返回 LCD panel handle 用于 direct draw_bitmap
esp_lcd_panel_handle_t Display::getPanelHandle() const {
  return panel_handle;
}

// 截图用：返回 LCD IO handle（用于 GRAM 读回）
esp_lcd_panel_io_handle_t Display::getIOHandle() const {
  return io_handle;
}

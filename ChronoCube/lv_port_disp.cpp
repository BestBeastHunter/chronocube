#include "config.h"

#ifdef USE_LVGL
#include "lv_port_disp.h"
#include "display.h"
#include "spi_bus_lock.h"
#include <esp_lcd_panel_ops.h>
#include "esp_heap_caps.h"
#include "lvgl.h"

extern Display display;

/* ==================== LVGL Display Driver (v9.5) ==================== */

/* Double draw buffer (matches official Waveshare 09_LVGL_V9_Test bsp_lvgl_port.cpp:172).
 * 480 x 12 rows x 2 bytes/pixel = 11,520 bytes per buffer.
 * 2 buffers = 23 KB DMA.
 *
 * BUF_ROWS 从 50→20→12：官方 BSP 用 50（96KB），但其仅跑 LVGL；
 * ChronoCube 并发 WiFi/音频/SD，DMA 池仅 ~234KB，需给其他模块留空间。
 * 12 行/帧 = 40 tiles/帧，对静态 UI（计时器/待机）完全够用。 */
#define BUF_ROWS   12
#define BUF_W      LCD_H_RES

static uint8_t       *buf1_dma    = NULL;  /* DMA heap, 480x12x2 = 11.5KB */
static uint8_t       *buf2_dma    = NULL;  /* DMA heap, 480x12x2 = 11.5KB */
static lv_display_t  *s_disp      = NULL;

/* Screen on/off */
static bool lvglScreenEnabled = true;

void lvglSetScreenEnabled(bool enabled) {
  lvglScreenEnabled = enabled;            // 停/恢复 LVGL flush（省 SPI2 带宽 + 电）
  /* 真正熄屏/亮屏：仅切面板亮度（0x51）。息屏=亮度0 全黑，GRAM 保留；
   * 亮屏=恢复全局亮度，画面瞬间恢复，无需 lv_obj_invalidate 全屏重绘。 */
  display.setBrightnessEnabled(enabled);
}
bool lvglIsScreenEnabled(void)          { return lvglScreenEnabled; }

/* ==================== Rounder Callback ==================== */

/**
 * Aligns invalidated areas to even pixel boundaries.
 * REQUIRED by SH8601/CO5300 QSPI: 2 pixels per transfer minimum.
 */
static void disp_rounder_cb(lv_event_t *e) {
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
  area->x1 = area->x1 & ~1;
  area->y1 = area->y1 & ~1;
  area->x2 = (area->x2 & ~1) + 1;
  area->y2 = (area->y2 & ~1) + 1;
  /* Clamp to screen bounds -- small edge tiles can round past the edge. */
  if (area->x2 >= LCD_H_RES) area->x2 = LCD_H_RES - 1;
  if (area->y2 >= LCD_V_RES) area->y2 = LCD_V_RES - 1;
}

/* ==================== Flush Callback ==================== */

static bool s_flushFirst = true;
static lvgl_flush_override_cb_t s_flushOverrideCb = NULL;

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  /* Screenshot capture mode */
  if (s_flushOverrideCb) {
    s_flushOverrideCb(disp, area, px_map);
    lv_display_flush_ready(disp);
    return;
  }

  if (!lvglScreenEnabled) {
    lv_display_flush_ready(disp);
    return;
  }

  int32_t x1 = area->x1, y1 = area->y1;
  int32_t x2 = area->x2, y2 = area->y2;
  int32_t w  = x2 - x1 + 1;
  int32_t h  = y2 - y1 + 1;

  /* Safety: skip degenerate areas that rounder may produce at edges */
  if (w <= 0 || h <= 0) {
    lv_display_flush_ready(disp);
    return;
  }

  if (s_flushFirst) {
    s_flushFirst = false;
    Serial.printf("[LVGL] FIRST FLUSH: (%d,%d)-(%d,%d) w=%d h=%d\n",
                  (int)x1, (int)y1, (int)x2, (int)y2, (int)w, (int)h);
  }

  /* Byte swap handled by lv_refr.c via LV_COLOR_16_SWAP=1.
   * Single swap path only -- no manual call in flush callback. */

  /* P0: SPI2 总线互斥 — LCD QSPI 与 SD SPI 共享 SPI2_HOST */
  if (spi2_lock()) {
    esp_err_t ret = esp_lcd_panel_draw_bitmap(display.getPanelHandle(),
                                               x1, y1, x2 + 1, y2 + 1,
                                               (void *)px_map);
    // P1-5: 检查绘制返回值，失败时记录日志（SPI2 瞬时错误静默重试）
    if (ret != ESP_OK) {
      static unsigned long lastDrawErrLog = 0;
      if (millis() - lastDrawErrLog > 10000) {
        lastDrawErrLog = millis();
        Serial.printf("[LVGL] draw_bitmap err=0x%X at (%d,%d) w=%d h=%d\n",
                      (int)ret, (int)x1, (int)y1, (int)w, (int)h);
      }
    }
    spi2_unlock();
    lv_display_flush_ready(disp);
  } else {
    /* 锁超时：跳过本次 flush，下帧重试 */
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 5000) {
      lastLog = millis();
      Serial.println("[LVGL] WARN: spi2_lock timeout, tile will retry");
    }
    // 通知 LVGL 该 tile 未完成，标记 dirty 以便下次 tick 自动重绘
    lv_display_flush_ready(disp);
    lv_area_t retry_area;
    retry_area.x1 = x1;
    retry_area.x2 = x2;
    retry_area.y1 = y1;
    retry_area.y2 = y2;
    lv_obj_invalidate_area(lv_screen_active(), &retry_area);
  }
}

/* ==================== Init ==================== */

void lv_port_disp_init(void) {
  size_t buf_bytes = BUF_W * BUF_ROWS * sizeof(lv_color_t);
  bool dual = true;

  /* 尝试分配双缓冲；失败则退回单缓冲（11.5KB vs 23KB），避免 OOM 绿屏 */
  buf1_dma = (uint8_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
  buf2_dma = (uint8_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
  if (!buf1_dma || !buf2_dma) {
    Serial.printf("[LVGL] WARN: dual DMA alloc failed (%luKB), falling back to single\n",
                  (unsigned long)(buf_bytes * 2) / 1024);
    heap_caps_free(buf2_dma); buf2_dma = NULL;
    if (!buf1_dma) {
      /* 连单缓冲都分配不到 → 真的 FATAL */
      Serial.println("[LVGL] FATAL: even single DMA buffer alloc failed");
      return;
    }
    dual = false;
  }
  memset(buf1_dma, 0, buf_bytes);
  if (buf2_dma) memset(buf2_dma, 0, buf_bytes);

  /* Create display */
  s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
  if (!s_disp) {
    Serial.println("[LVGL] FATAL: display create failed");
    heap_caps_free(buf1_dma); buf1_dma = NULL;
    heap_caps_free(buf2_dma); buf2_dma = NULL;
    return;
  }

  lv_display_set_flush_cb(s_disp, disp_flush_cb);

  if (dual) {
    lv_display_set_buffers(s_disp, buf1_dma, buf2_dma, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
  } else {
    lv_display_set_buffers(s_disp, buf1_dma, NULL, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
  }

  /* Rounder callback: align areas to even boundaries (QSPI requirement).
   * Must be set AFTER set_buffers / set_flush_cb. */
  lv_display_add_event_cb(s_disp, disp_rounder_cb,
                          LV_EVENT_INVALIDATE_AREA, NULL);

  lvglScreenEnabled = true;
  s_flushFirst = true;

  /* Runtime verification: confirm PARTIAL mode is active */
  lv_display_render_mode_t actualMode = lv_display_get_render_mode(s_disp);
  const char *modeStr = (actualMode == LV_DISPLAY_RENDER_MODE_PARTIAL) ? "PARTIAL" :
                         (actualMode == LV_DISPLAY_RENDER_MODE_DIRECT) ? "DIRECT" :
                         (actualMode == LV_DISPLAY_RENDER_MODE_FULL) ? "FULL" : "UNKNOWN";
  unsigned long totalDmaKB = (buf_bytes * (dual ? 2 : 1)) / 1024;
  Serial.printf("[LVGL] disp init: %dx%d, draw_buf=%dx%d x%d (DMA %luKB), "
                "rounder=even, render=%s, v9.5\n",
                LCD_H_RES, LCD_V_RES, BUF_W, BUF_ROWS, dual ? 2 : 1,
                totalDmaKB, modeStr);

  if (actualMode != LV_DISPLAY_RENDER_MODE_PARTIAL) {
    Serial.printf("[LVGL] FATAL: render mode is %s, expected PARTIAL!\n", modeStr);
  }
}

void lvglSetFlushOverride(lvgl_flush_override_cb_t cb) {
  s_flushOverrideCb = cb;
}

#endif /* USE_LVGL */

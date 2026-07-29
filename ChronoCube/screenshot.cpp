#include "config.h"

#ifdef USE_LVGL

#include "screenshot.h"
#include "lvgl.h"
#include "display.h"
#include "lv_port_disp.h"  /* lvglSetFlushOverride */

// ============================================================
// Flush-intercept screenshot approach (LVGL v9.5)
//
// ESP32-C6 has only ~258KB DRAM. A full 480x480 RGB565 frame
// (460KB) cannot fit. Instead, we intercept LVGL's normal
// flush callback — each flush tile ≤ 122KB (480×128 pixels,
// matching lv_draw_buf_init draw buffer).
//
// Protocol (ESP32 -> PC):
//   Header: 0xCC 0xCC [4B total_bytes LE] [2B width LE] [2B height LE]
//   Data:   480 rows × 960 bytes (RGB565, native byte order)
// ============================================================

#ifndef SHOT_BUF_ROWS
#define SHOT_BUF_ROWS 128  /* must match BUF_ROWS in lv_port_disp.cpp */
#endif

#define SHOT_MAGIC      0xCC
#define SHOT_WIDTH      480
#define SHOT_HEIGHT     480
#define SHOT_ROW_BYTES  (SHOT_WIDTH * 2)  // 960
#define SHOT_TOTAL      (SHOT_WIDTH * SHOT_HEIGHT * 2)  // 460800

static bool s_capturing = false;
static bool s_shotPending = false;
static int  s_rowSent = 0;

void screenshotInit() {
  // DISABLED: screenshot capture conflicts with SH8601 QSPI flush pipeline
  // Removing flush-override init until QSPI state issue is resolved.
  // Serial.println("[SHOT] flush-intercept mode ready (v9.5)");
  // Serial.println("[SHOT] send 'shot' to capture");
  return;
}

// Intercept flush via lvglSetFlushOverride: wrapper (disp_flush_cb) calls
// lv_display_flush_ready() after this callback returns, so we must NOT
// call it ourselves.
static void capture_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  if (!s_capturing) return;

  int w = area->x2 - area->x1 + 1;
  int h = area->y2 - area->y1 + 1;
  int totalBytes = w * h * 2;

  // Write pixel data to serial (for PC screenshot tool)
  if (Serial.write(px_map, totalBytes) != (size_t)totalBytes) {
    s_capturing = false;
    return;
  }

  s_rowSent += h;

  // Safety split: draw to physical display in chunks if flush exceeds
  // expected tile size (defense-in-depth, matches lv_port_disp.cpp logic).
  int stride = w * 2;
  if (h > SHOT_BUF_ROWS) {
    for (int y_off = 0; y_off < h; y_off += SHOT_BUF_ROWS) {
      int chunk_h = (y_off + SHOT_BUF_ROWS <= h) ? SHOT_BUF_ROWS : (h - y_off);
      esp_lcd_panel_draw_bitmap(display.getPanelHandle(),
          area->x1, area->y1 + y_off, area->x2 + 1, area->y1 + y_off + chunk_h,
          (void *)(px_map + y_off * stride));
    }
  } else {
    esp_lcd_panel_draw_bitmap(display.getPanelHandle(),
        area->x1, area->y1, area->x2 + 1, area->y2 + 1, (void *)px_map);
  }
}

static bool sendScreenshot() {
  // Send protocol header
  uint8_t header[10];
  header[0] = SHOT_MAGIC;
  header[1] = SHOT_MAGIC;
  header[2] = (SHOT_TOTAL >> 0)  & 0xFF;
  header[3] = (SHOT_TOTAL >> 8)  & 0xFF;
  header[4] = (SHOT_TOTAL >> 16) & 0xFF;
  header[5] = (SHOT_TOTAL >> 24) & 0xFF;
  header[6] = (SHOT_WIDTH >> 0) & 0xFF;
  header[7] = (SHOT_WIDTH >> 8) & 0xFF;
  header[8] = (SHOT_HEIGHT >> 0) & 0xFF;
  header[9] = (SHOT_HEIGHT >> 8) & 0xFF;

  if (Serial.write(header, 10) != 10) {
    Serial.println("[SHOT] ERROR: header write failed");
    return false;
  }

  // Install capture override (v9: use flush override instead of touching flush_cb directly)
  s_capturing = true;
  s_rowSent = 0;
  lvglSetFlushOverride(capture_flush_cb);

  // Force full redraw
  // v9: lv_screen_active() replaces lv_scr_act()
  lv_obj_invalidate(lv_screen_active());

  // Wait for LVGL to process the redraw (~10 chunks × 30ms = 300ms)
  unsigned long deadline = millis() + 5000;
  while (s_capturing && millis() < deadline) {
    lv_timer_handler();
    delay(5);
  }

  // Restore normal flush
  lvglSetFlushOverride(NULL);

  Serial.flush();

  if (s_rowSent < SHOT_HEIGHT) {
    Serial.printf("[SHOT] WARNING: only %d/%d rows captured\n", s_rowSent, SHOT_HEIGHT);
    return false;
  }

  return true;
}

void screenshotTrigger() {
  s_shotPending = true;
}

void screenshotTick() {
  // DISABLED: screenshot auto-trigger breaks SH8601 QSPI display state
  // Serial RX on ESP32-C6 USB-CDC is also broken — no way to trigger via serial.
  // TODO: fix QSPI state corruption in capture_flush_cb before re-enabling.
  return;
}

#endif // USE_LVGL

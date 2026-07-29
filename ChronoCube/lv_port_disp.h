#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#include "config.h"

#ifdef USE_LVGL

#include "lvgl.h"

void lv_port_disp_init(void);

void lvglSetScreenEnabled(bool enabled);
bool lvglIsScreenEnabled(void);

/* ==================== Flush Override (for screenshot capture) ====================
 * Set a callback that intercepts all LVGL flush operations.
 * When set, pixels are NOT sent to LCD — the callback receives them instead.
 * Pass NULL to clear the override and resume normal LCD drawing.
 * The callback MUST NOT call lv_display_flush_ready() — the wrapper handles it.
 *
 * LVGL v9 signature: px_map is uint8_t* (not lv_color_t*).
 * The byte order follows LV_COLOR_16_SWAP setting. */
typedef void (*lvgl_flush_override_cb_t)(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
void lvglSetFlushOverride(lvgl_flush_override_cb_t cb);

#endif /* USE_LVGL */

#endif /* LV_PORT_DISP_H */

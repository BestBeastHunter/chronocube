#include "config.h"

#ifdef USE_LVGL
#include "lv_port_indev.h"
#include "touch.h"

extern TouchPanel touch;

/**
 * LVGL touch read callback (v9 signature — same as v8).
 */
static void touch_read_cb(lv_indev_t *drv, lv_indev_data_t *data) {
  static int16_t lastX = 0;
  static int16_t lastY = 0;

  uint16_t x, y;
  bool pressed = touch.read(&x, &y);

  if (pressed) {
    lastX = (int16_t)x;
    lastY = (int16_t)y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }

  data->point.x = lastX;
  data->point.y = lastY;
}

/**
 * LVGL v9 input device initialization.
 * v8: lv_indev_drv_register()
 * v9: lv_indev_create() + configure
 */
void lv_port_indev_init(void) {
  lv_indev_t *indev = lv_indev_create();
  if (!indev) {
    Serial.println("[LVGL] FATAL: indev create failed");
    return;
  }
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);
  Serial.println("[LVGL] indev init OK (v9.5)");
}
#endif /* USE_LVGL */

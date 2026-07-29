#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#include "config.h"

#ifdef USE_LVGL
#include "lvgl.h"

void lv_port_indev_init(void);
#endif /* USE_LVGL */

#endif
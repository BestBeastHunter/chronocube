/**
 * @file spi_bus_lock.cpp
 * SPI2 总线互斥锁实现 — FreeRTOS 互斥信号量
 */

#include "spi_bus_lock.h"
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_spi2_mutex = NULL;

void spi2_lock_init(void) {
  if (s_spi2_mutex == NULL) {
    s_spi2_mutex = xSemaphoreCreateRecursiveMutex();  // 递归锁：防止同任务重入死锁
  }
}

bool spi2_lock(const char *tag) {
  if (s_spi2_mutex == NULL) return true;  /* 未初始化则放行 */
  bool ok = xSemaphoreTakeRecursive(s_spi2_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
#ifdef DEBUG_SERIAL
  if (!ok) {
    Serial.printf("[SPI2] LOCK FAIL by %s (timeout)\n", tag ? tag : "?");
  }
#endif
  return ok;
}

void spi2_unlock(const char *tag) {
  if (s_spi2_mutex == NULL) return;
  xSemaphoreGiveRecursive(s_spi2_mutex);
}

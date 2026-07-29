/**
 * @file spi_bus_lock.h
 * SPI2 总线互斥锁 — SD SPI 和 LCD QSPI 共享 SPI2_HOST 的保护机制
 *
 * 问题：SD 卡 (SPI 标准模式) 和 LCD (QSPI 四线模式) 共享 SPI2_HOST，
 *       通过不同 CS 区分，但无互斥保护。同时访问会导致数据错乱/总线冲突。
 *
 * 使用：spi2_lock()   — 获取锁 (fread SD / esp_lcd_panel_draw_bitmap 前)
 *       spi2_unlock() — 释放锁
 *       spi2_lock_init() — 在 setup() 中调用一次
 *
 * 实现：FreeRTOS 互斥信号量，支持 ISR 竞争场景。
 */

#ifndef SPI_BUS_LOCK_H
#define SPI_BUS_LOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 SPI2 总线互斥锁。
 * 应在 display.begin() + storage.begin() 之后调用。
 */
void spi2_lock_init(void);

/**
 * 获取 SPI2 总线锁。
 * 阻塞等待，超时 100ms 后返回 false（表示 SPI 总线异常）。
 * @param tag 调用者标识（调试用，"?" 为未标记的旧调用方）
 * @return true=获取成功, false=超时
 */
bool spi2_lock(const char *tag = "?");

/**
 * 释放 SPI2 总线锁。
 * @param tag 调用者标识（调试用，"?" 为未标记的旧调用方）
 */
void spi2_unlock(const char *tag = "?");

#ifdef __cplusplus
}
#endif

#endif /* SPI_BUS_LOCK_H */

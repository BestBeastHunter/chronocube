#ifndef I2C_BSP_H
#define I2C_BSP_H

#include <Arduino.h>
#include <Wire.h>

// ==================== 统一 I2C 总线封装（基于 Wire.h）====================
// 文档 3.1/9.1 强制要求所有 I2C 外设统一走 Arduino 标准 Wire 接口，
// 避免与官方库产生总线冲突。MVP 阶段只挂 I2C0 一条总线。
class I2CBus {
public:
  // 初始化总线（必须在任何 I2C 外设 begin 之前调用一次）
  static bool begin(int sda = 8, int scl = 7, uint32_t freq = 400000);

  // 写 N 字节到寄存器
  static int writeReg(uint8_t addr, uint8_t reg, const uint8_t *buf, uint8_t len);
  // 写 1 字节到寄存器
  static int writeReg8(uint8_t addr, uint8_t reg, uint8_t value);
  // 读 N 字节
  static int readReg(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len);
  // 读 1 字节
  static int readReg8(uint8_t addr, uint8_t reg, uint8_t *value);
  // 多字节寄存器地址读（如 CST9220 的 2 字节寄存器 0xD0,0x00）
  static int readRegAddr(uint8_t addr, const uint8_t *reg, uint8_t regLen,
                         uint8_t *buf, uint8_t bufLen);

  // 总线扫描，用于诊断
  static void scan(Stream &out = Serial);

  // 探测某个地址是否有 ACK
  static bool probe(uint8_t addr);
};

#endif // I2C_BSP_H

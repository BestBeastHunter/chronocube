#include "touch.h"
#include "config.h"
#include "i2c_bsp.h"
#include <Arduino.h>

TouchPanel touch;

// CST9220 触摸寄存器读法（与微雪官方 lcd_touch.cpp 一致）
static const uint8_t CST9220_REG[2] = { 0xD0, 0x00 };

bool TouchPanel::begin() {
  // 硬件复位
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(50);
  digitalWrite(PIN_TP_RST, LOW);
  delay(50);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(200);

  // 探测一下
  if (!I2CBus::probe(I2C_ADDR_CST9220)) {
    Serial.println("[TP] CST9220 not found at 0x5A");
    return false;
  }
  Serial.println("[TP] CST9220 OK");
  fingerDown = false;
  fingerDownMs = 0;
  rotation = 0;
  return true;
}

bool TouchPanel::read(uint16_t *outX, uint16_t *outY) {
  uint8_t data[10] = {0};
  /* 通过 I2CBus 统一接口读取 CST9220：写 2 字节寄存器地址 0xD0,0x00，读 10 字节 */
  int rc = I2CBus::readRegAddr(I2C_ADDR_CST9220, CST9220_REG, 2, data, 10);
  // P1-5: I2C 读取失败时重置触摸状态，防止 fingerDown 残留导致 isFingerUp() 误判
  if (rc != 0) { fingerDown = false; return false; }

  // 校验 magic
  if (data[6] != 0xAB) return false;
  uint8_t points = data[5] & 0x7F;
  if (points == 0) { fingerDown = false; return false; }

  uint8_t status = data[0] & 0x0F;
  if (status != 0x06) return false;

  uint16_t x = ((uint16_t)data[1] << 4) | (data[3] >> 4);
  uint16_t y = ((uint16_t)data[2] << 4) | (data[3] & 0x0F);

  // 坐标缩放：CST9220 为 12-bit(0~4095) 原生输出时缩放到显示分辨率；
  // 若原生已是 480，则 x/y 均 < LCD_*，此项为 no-op（不影响）。
  if (x >= LCD_H_RES) x = (uint16_t)((uint32_t)x * LCD_H_RES / 4096);
  if (y >= LCD_V_RES) y = (uint16_t)((uint32_t)y * LCD_V_RES / 4096);

  // 触控 Y 与显示镜像；X 与显示同向（实测 X 镜像会导致滑块反向），故只翻转 Y
  y = LCD_V_RES - 1 - y;

  // 根据屏幕旋转角度做坐标逆变换
  // 屏幕顺时针转了 θ，触控坐标要逆时针转 θ 才能对应显示坐标
  uint16_t tx = x, ty = y;
  switch (rotation) {
    case 90:   // 屏幕顺时针90° → 触控逆时针90°
      tx = LCD_V_RES - 1 - y;
      ty = x;
      break;
    case 180:  // 屏幕顺时针180° → 触控逆时针180°
      tx = LCD_H_RES - 1 - x;
      ty = LCD_V_RES - 1 - y;
      break;
    case 270:  // 屏幕顺时针270° → 触控逆时针270° = 顺时针90°
      tx = y;
      ty = LCD_H_RES - 1 - x;
      break;
    default:   // 0° 不变
      break;
  }
  x = tx; y = ty;

  // 边界保护
  if (x >= LCD_H_RES) x = LCD_H_RES - 1;
  if (y >= LCD_V_RES) y = LCD_V_RES - 1;

  if (outX) *outX = x;
  if (outY) *outY = y;

  if (!fingerDown) { fingerDown = true; fingerDownMs = millis(); }
  return true;
}

bool TouchPanel::isFingerUp() { return !fingerDown; }

bool TouchPanel::waitClick(uint16_t *outX, uint16_t *outY, uint32_t timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (read(outX, outY)) {
      // 等待手指抬起才算一次完整 click
      while (fingerDown) { delay(5); read(nullptr, nullptr); if (millis() - start > timeoutMs) break; }
      return true;
    }
    delay(10);
  }
  return false;
}

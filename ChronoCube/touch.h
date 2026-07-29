#ifndef TOUCH_H
#define TOUCH_H

#include <Arduino.h>
#include <stdint.h>

// CST9220 电容触控（I2C 0x5A）
// 文档 2.2.2 / 4.3
class TouchPanel {
public:
  bool begin();                       // 初始化触控（包含硬件复位）
  // 读取一次坐标。有触摸返回 true，并把 (x,y) 写入 outX/outY
  bool read(uint16_t *outX, uint16_t *outY);
  // 阻塞等待一次有效点击，超时返回 false
  bool waitClick(uint16_t *outX, uint16_t *outY, uint32_t timeoutMs = 0xFFFFFFFF);
  bool isFingerUp();                  // 手指是否抬起

  // 设置旋转角度（与屏幕旋转同步）
  void setRotation(uint16_t deg) { rotation = deg; }
  uint16_t getRotation() { return rotation; }

private:
  bool fingerDown;                    // 当前是否处于按下状态
  unsigned long fingerDownMs;
  uint16_t rotation;                  // 0/90/180/270
};

extern TouchPanel touch;

#endif

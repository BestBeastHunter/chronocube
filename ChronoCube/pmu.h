#ifndef PMU_H
#define PMU_H

#include <Arduino.h>
#include <stdint.h>
#include "config.h"

// AXP2101 电源管理单例
// 文档 2.2.2 / 4.8
class PowerManager {
public:
  bool begin();                                     // 初始化 AXP2101 并打开外设电源
  void enableLcdPower(bool on);                     // ALDO3 控制 LCD 复位/上电
  void enableAudioPower(bool on);                   // ALDO2 控制音频功放
  bool isCharging();                                // 是否正在充电
  uint8_t getBatteryPercent();                      // 电量百分比（估算）
  float getBatteryVoltage();                        // 电池电压(V)
  float getVbusVoltage();                           // USB 电压(V)

  // ---- 功耗分级（TASK-A4）----
  void setPowerLevel(PowerLevel level);
  PowerLevel getPowerLevel();
  // L2/L3 唤醒钩子：setPowerLevel 回到 L0 且之前等级 >= L2 时触发（用于 LCD 重初始化）
  void setWakeCallback(void (*cb)());

private:
  void (*wakeCb)() = nullptr;           // L2/L3→L0 唤醒时回调（如 display.reinitAfterWake）
};

extern PowerManager powerManager;

#endif

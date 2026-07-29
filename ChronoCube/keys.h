#ifndef KEYS_H
#define KEYS_H

#include <Arduino.h>
#include <stdint.h>

// 按键事件
enum KeyEvent {
  KEY_EVT_NONE = 0,
  KEY_EVT_BOOT_CLICK,    // BOOT 单击（停止提醒音）
  KEY_EVT_BOOT_LONG,     // BOOT 长按 2s（切换锁定）
  KEY_EVT_USER_CLICK,    // USER 单击（亮屏显示当日总时长）
  KEY_EVT_USER_LONG      // USER 长按 2s（切换静音）
  // PWR 按键保留原生电源功能，不做自定义
};

// 物理按键扫描（文档 4.5）
class KeyManager {
public:
  void begin();
  KeyEvent update();    // 主循环调用，返回新产生的事件
  bool isLocked();      // 锁定状态
  void setLocked(bool l);
  bool isMuted();       // 静音状态
  void setMuted(bool m);
  void toggleMute();

  // 运行时配置（由 config_loader 设置）
  void setLongpressMs(unsigned long ms) { longpressMs = ms; }

private:
  bool locked;
  bool muted;
  unsigned long bootDownMs;
  unsigned long userDownMs;
  bool bootPrevDown;
  bool userPrevDown;
  bool bootLongFired;
  bool userLongFired;
  unsigned long longpressMs;  // 运行时覆盖 KEY_LONGPRESS_MS
};

extern KeyManager keys;

#endif

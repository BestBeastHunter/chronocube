#include "keys.h"
#include "config.h"

KeyManager keys;

void KeyManager::begin() {
  pinMode(PIN_KEY_BOOT, INPUT_PULLUP);
  pinMode(PIN_KEY_USER, INPUT_PULLUP);
  pinMode(PIN_KEY_PWR,  INPUT_PULLUP);
  bootDownMs = userDownMs = 0;
  bootPrevDown = userPrevDown = false;
  bootLongFired = userLongFired = false;
  locked = true;  // 默认锁定，与开机行为一致
  muted = false;
  longpressMs = KEY_LONGPRESS_MS;  // 固件默认值
}

bool KeyManager::isLocked() { return locked; }
void KeyManager::setLocked(bool l) { locked = l; }
bool KeyManager::isMuted() { return muted; }
void KeyManager::setMuted(bool m) { muted = m; }
void KeyManager::toggleMute() { muted = !muted; }

KeyEvent KeyManager::update() {
  unsigned long now = millis();
  bool bootDown = (digitalRead(PIN_KEY_BOOT) == LOW);
  bool userDown = (digitalRead(PIN_KEY_USER) == LOW);
  KeyEvent evt = KEY_EVT_NONE;

  // BOOT 键
  if (bootDown && !bootPrevDown) {
    bootDownMs = now;
    bootLongFired = false;
  } else if (bootDown && !bootLongFired) {
    if (now - bootDownMs >= longpressMs) {
      bootLongFired = true;
      evt = KEY_EVT_BOOT_LONG;
    }
  } else if (!bootDown && bootPrevDown) {
    if (!bootLongFired && (now - bootDownMs) > 50) {
      evt = KEY_EVT_BOOT_CLICK;  // 短按
    }
  }
  bootPrevDown = bootDown;

  // USER 键
  if (userDown && !userPrevDown) {
    userDownMs = now;
    userLongFired = false;
  } else if (userDown && !userLongFired) {
    if (now - userDownMs >= longpressMs) {
      userLongFired = true;
      evt = KEY_EVT_USER_LONG;
    }
  } else if (!userDown && userPrevDown) {
    if (!userLongFired && (now - userDownMs) > 50) {
      evt = KEY_EVT_USER_CLICK;
    }
  }
  userPrevDown = userDown;

  // NOTE: If BOOT and USER both fire in the same frame, USER wins (BOOT event is lost).
  // This is inherent to the single-return-value design; simultaneous dual-key presses
  // are an edge case not expected in normal use.
  return evt;
}

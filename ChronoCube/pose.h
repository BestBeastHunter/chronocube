#ifndef POSE_H
#define POSE_H

#include <Arduino.h>
#include "qmi8658.h"
#include "config.h"

// 姿态面（物理姿态，与业务状态解耦）
enum PoseFace {
  POSE_FLAT_UP = 0,    // 平放（屏幕朝上）
  POSE_FLAT_DOWN,      // 反扣（屏幕朝下）
  POSE_UPRIGHT,        // 直立（底面朝下）
  POSE_LEFT,           // 左侧立
  POSE_RIGHT,          // 右侧立
  POSE_INVERTED,       // 倒立（顶面朝下，暂不启用）
  POSE_UNKNOWN,        // 不稳定/在运动
};

// 姿态检测结果
struct PoseEvent {
  PoseFace face;       // 当前稳定面
  bool stable;         // 是否稳定（二阶确认后）
  bool preConfirm;     // 一阶预反馈触发（200ms）
  unsigned long faceStartMs; // 当前面的起始时间
};

class PoseDetector {
public:
  bool begin();
  void update();                                // 主循环 ~50Hz 调用

  PoseFace getFace() { return currentFace; }
  PoseFace getRawFace() { return lastRawFace; }  // 即时判据（未防抖），用于 flipDownSince 等
  bool isStable() { return stable; }
  bool isPreConfirmed() { return preConfirmed; }
  void clearPreConfirmed() { preConfirmed = false; }
  unsigned long getFaceDurationMs() { return stable ? (millis() - faceStartMs) : 0; }

  // 运行时配置（由 config_loader 设置，覆盖 config.h 宏默认值）
  void setAngleThreshold(float deg)   { angleThreshold = deg; }
  void setConfirmMs(unsigned long ms)  { confirmMsDefault = ms; }
  void setFacedownConfirmMs(unsigned long ms) { facedownConfirmMs = ms; }
  void setFlipFastMs(unsigned long ms) { flipFastMs = ms; }
  void setPredelayMs(unsigned long ms) { predelayMs = ms; }
  void setGyroFlipDps(float dps)      { gyroFlipDps = dps; }
  void setGyroStillDps(float dps)     { gyroStillDps = dps; }
  void setMotionFilterG(float g)      { motionFilterG = g; }

  // IMU ODR 运行时切换（L2 省电 / 唤醒恢复）
  void setAccelOdrLowPower() { qmi8658_set_accel_odr(&imuDev, QMI8658_ACCEL_ODR_LOWPOWER_3HZ); }
  void setAccelOdrNormal()   { qmi8658_set_accel_odr(&imuDev, QMI8658_ACCEL_ODR_1000HZ); }

  float accX, accY, accZ;
  float gyroX, gyroY, gyroZ;            // 陀螺仪角速度 (°/s)，TASK-007
  unsigned long lastChangeMs;

private:
  qmi8658_dev_t imuDev;
  PoseFace currentFace;
  PoseFace pendingFace;
  PoseFace lastRawFace;
  unsigned long rawChangeMs;
  unsigned long confirmMs;
  unsigned long faceStartMs;
  bool stable;
  bool preConfirmed;

  // 运行时可覆盖参数
  float angleThreshold;
  unsigned long confirmMsDefault;
  unsigned long facedownConfirmMs;
  unsigned long flipFastMs;
  unsigned long predelayMs;
  float gyroFlipDps;
  float gyroStillDps;
  float motionFilterG;

  PoseFace classify(float ax, float ay, float az);
  bool motionFilter(float ax, float ay, float az);
  unsigned long getConfirmMs(PoseFace f);
  bool isFliping();                      // TASK-007: 检测是否在快速翻转
  bool isStill();                        // TASK-007: 检测是否静止（角速度近零）
};

extern PoseDetector poseDetector;

#endif // POSE_H

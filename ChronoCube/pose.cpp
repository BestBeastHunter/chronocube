#include "pose.h"
#include "config.h"
#include <math.h>

PoseDetector poseDetector;

bool PoseDetector::begin() {
  esp_err_t ret = qmi8658_init(&imuDev, I2C_ADDR_QMI8658_H);
  if (ret != ESP_OK) return false;

  qmi8658_set_accel_unit_mg(&imuDev, true);
  // 陀螺仪单位保持默认 °/s（不设 mps2/rads）

  // 初始化运行时参数为 config.h 默认值
  angleThreshold    = POSE_ANGLE_THRESHOLD;
  confirmMsDefault  = POSE_CONFIRM_MS;
  facedownConfirmMs = POSE_FACEDOWN_CONFIRM_MS;
  flipFastMs        = POSE_FLIP_FAST_MS;
  predelayMs        = POSE_PREDELAY_MS;
  gyroFlipDps       = POSE_GYRO_FLIP_DPS;
  gyroStillDps      = POSE_GYRO_STILL_DPS;
  motionFilterG     = POSE_MOTION_FILTER_G;

  accX = accY = 0; accZ = 1.0f;
  gyroX = gyroY = gyroZ = 0;
  currentFace   = POSE_FLAT_UP;
  pendingFace   = POSE_FLAT_UP;
  lastRawFace   = POSE_FLAT_UP;
  rawChangeMs   = 0;
  confirmMs     = 0;
  faceStartMs   = millis();
  stable        = true;
  preConfirmed  = false;
  lastChangeMs  = millis();

  float x, y, z;
  if (qmi8658_read_accel(&imuDev, &x, &y, &z) == ESP_OK) {
    accX = x / 1000.0f; accY = y / 1000.0f; accZ = z / 1000.0f;
    currentFace = classify(accX, accY, accZ);
    if (currentFace == POSE_UNKNOWN) currentFace = POSE_FLAT_UP;
  }
  return true;
}

bool PoseDetector::motionFilter(float ax, float ay, float az) {
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float diff = fabsf(mag - 1.0f);
  if (diff > motionFilterG) return true;
  if (mag < POSE_RAW_VALID_MIN || mag > POSE_RAW_VALID_MAX) return true;
  return false;
}

PoseFace PoseDetector::classify(float ax, float ay, float az) {
  // 6 个面的法向量
  // 注意：QMI8658 实际输出与定义相反，三个轴全部取反后再匹配
  float nx = -ax, ny = -ay, nz = -az;
  struct Face { float nx, ny, nz; PoseFace f; };
  static const Face faces[6] = {
    { 0,  0,  1, POSE_FLAT_UP   },   // 平放 z+
    { 0,  0, -1, POSE_FLAT_DOWN },   // 反扣 z-
    { 0, -1,  0, POSE_UPRIGHT   },   // 直立 y-  (底朝下)
    { 1,  0,  0, POSE_LEFT      },   // 左立 x+
    {-1,  0,  0, POSE_RIGHT     },   // 右立 x-
    { 0,  1,  0, POSE_INVERTED  },   // 倒立 y+  (顶朝下)
  };

  float best = 2.0f;
  PoseFace f = POSE_UNKNOWN;
  for (uint8_t i = 0; i < 6; i++) {
    float dot = nx*faces[i].nx + ny*faces[i].ny + nz*faces[i].nz;
    if (dot < 0) dot = 0;
    float loss = 1.0f - dot;
    if (loss < best) { best = loss; f = faces[i].f; }
  }
  if (best > (1.0f - cosf(angleThreshold * (PI/180.0f)))) {
    return POSE_UNKNOWN;
  }
  return f;
}

unsigned long PoseDetector::getConfirmMs(PoseFace f) {
  // 反扣特殊：可配置确认时间（防误触）
  if (f == POSE_FLAT_DOWN) return facedownConfirmMs;
  // 快速翻转中：缩短确认时间（TASK-007）
  if (isFliping()) return flipFastMs;
  // 静止时：加速确认（TASK-007）
  if (isStill()) {
    // 减少 30%，但不低于 flipFastMs
    unsigned long fast = confirmMsDefault * 7 / 10;
    if (fast < flipFastMs) fast = flipFastMs;
    return fast;
  }
  // 其他面：标准确认时间
  return confirmMsDefault;
}

// TASK-007: 检测是否在快速翻转（角速度矢量和 > 阈值）
bool PoseDetector::isFliping() {
  float gmag = sqrtf(gyroX*gyroX + gyroY*gyroY + gyroZ*gyroZ);
  return gmag > gyroFlipDps;
}

// TASK-007: 检测是否静止（角速度矢量和 < 阈值）
bool PoseDetector::isStill() {
  float gmag = sqrtf(gyroX*gyroX + gyroY*gyroY + gyroZ*gyroZ);
  return gmag < gyroStillDps;
}

void PoseDetector::update() {
  float x, y, z;
  esp_err_t ret = qmi8658_read_accel(&imuDev, &x, &y, &z);
  if (ret != ESP_OK) return;

  accX = x / 1000.0f;
  accY = y / 1000.0f;
  accZ = z / 1000.0f;

  // TASK-007: 同时读取陀螺仪角速度
  float gx, gy, gz;
  if (qmi8658_read_gyro(&imuDev, &gx, &gy, &gz) == ESP_OK) {
    gyroX = gx; gyroY = gy; gyroZ = gz;
  }

  unsigned long now = millis();

  bool moving = motionFilter(accX, accY, accZ);

  PoseFace raw;
  if (moving) {
    // TASK-007: 运动中但陀螺仪显示静止 → 可能是慢速倾斜，仍尝试分类
    if (isStill()) {
      raw = classify(accX, accY, accZ);
      if (raw == POSE_UNKNOWN) raw = lastRawFace;
    } else {
      raw = POSE_UNKNOWN;
    }
  } else {
    raw = classify(accX, accY, accZ);
    if (raw == POSE_UNKNOWN) raw = lastRawFace;
  }

  // 防抖状态机
  if (raw != lastRawFace) {
    rawChangeMs = now;
    preConfirmed = false;
    confirmMs = 0;
    stable = false;
  } else {
    // 一阶：预反馈
    if (!preConfirmed && (now - rawChangeMs) >= predelayMs) {
      preConfirmed = true;
      pendingFace  = raw;
    }
    // 二阶：确认（反扣 2.5s，快速翻转 800ms，静止 1.05s，标准 1.5s）
    if (preConfirmed && raw == pendingFace) {
      unsigned long need = getConfirmMs(raw);
      if (confirmMs == 0) confirmMs = now;
      if ((now - confirmMs) >= need) {
        if (currentFace != raw) {
          currentFace  = raw;
          faceStartMs  = now;
          lastChangeMs = now;
        }
        stable       = true;
        preConfirmed = false;
        confirmMs    = 0;
      }
    }
  }

  lastRawFace = raw;
}

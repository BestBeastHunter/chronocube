#include "pmu.h"
#include "config.h"
#include "i2c_bsp.h"
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

static XPowersPMU axp;

// 锂电池放电曲线查表（电压 mV → SOC %）
// 基于典型 Li-ion 放电曲线，平台区（3.6~3.9V）密度更高
static const struct { uint16_t mv; uint8_t pct; } batt_lut[] = {
  { 3300,   0 },
  { 3500,   3 },
  { 3550,   5 },
  { 3600,  10 },
  { 3650,  15 },
  { 3700,  20 },
  { 3750,  30 },
  { 3800,  40 },
  { 3850,  50 },
  { 3900,  60 },
  { 3950,  70 },
  { 4000,  80 },
  { 4050,  88 },
  { 4100,  95 },
  { 4150,  98 },
  { 4200, 100 },
};
static const int LUT_SIZE = sizeof(batt_lut) / sizeof(batt_lut[0]);

// EMA 低通滤波（消除负载波动）
static uint32_t filteredMv = UINT32_MAX;  // UINT32_MAX = 未初始化，0 是有效值
static const int EMA_ALPHA = 20;  // 20% 新值 + 80% 旧值

static int _pmu_read(uint8_t dev, uint8_t reg, uint8_t *data, uint8_t len) {
  return I2CBus::readReg(dev, reg, data, len);
}

static int _pmu_write(uint8_t dev, uint8_t reg, uint8_t *data, uint8_t len) {
  return I2CBus::writeReg(dev, reg, data, len);
}

PowerManager powerManager;

bool PowerManager::begin() {
  if (!axp.begin(I2C_ADDR_AXP2101, _pmu_read, _pmu_write)) {
    Serial.println("[PMU] AXP2101 not found");
    return false;
  }
  Serial.println("[PMU] AXP2101 OK");

  // 文档 4.8：开 DCDC1 + 4 个 ALDO
  axp.setDC1Voltage(3300);
  axp.setALDO1Voltage(3300);
  axp.setALDO2Voltage(3300);   // 音频功放
  axp.setALDO3Voltage(3300);   // LCD 电源
  axp.setALDO4Voltage(3300);

  // 充电参数：先设限流再开 rails（官方示例顺序，防 USB 瞬时电流过大）
  axp.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_2000MA);
  axp.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
  axp.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
  axp.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_50MA);

  axp.enableDC1();
  axp.enableALDO1();
  axp.enableALDO2();
  axp.enableALDO3();
  axp.enableALDO4();

  return true;
}

void PowerManager::enableLcdPower(bool on) {
  if (on) axp.enableALDO3(); else axp.disableALDO3();
}

void PowerManager::enableAudioPower(bool on) {
  if (on) axp.enableALDO2(); else axp.disableALDO2();
}

bool PowerManager::isCharging() {
  return axp.isCharging();
}

uint8_t PowerManager::getBatteryPercent() {
  uint16_t rawMv = (uint16_t)axp.getBattVoltage();  // mV

  // 边界处理
  if (rawMv <= 3300) { filteredMv = (uint32_t)rawMv; return 0; }
  if (rawMv >= 4200) { filteredMv = 4200; return 100; }

  // EMA 低通滤波（消除负载波动导致的电压抖动）
  if (filteredMv == UINT32_MAX) {
    filteredMv = (uint32_t)rawMv;  // 首次调用，直接赋值
  } else {
    filteredMv = (EMA_ALPHA * (uint32_t)rawMv + (100 - EMA_ALPHA) * filteredMv) / 100;
  }

  uint16_t mv = (uint16_t)filteredMv;

  // 查表线性插值
  for (int i = 0; i < LUT_SIZE - 1; i++) {
    if (mv <= batt_lut[i + 1].mv) {
      uint16_t vLo = batt_lut[i].mv;
      uint16_t vHi = batt_lut[i + 1].mv;
      uint8_t  pLo = batt_lut[i].pct;
      uint8_t  pHi = batt_lut[i + 1].pct;
      return pLo + (uint8_t)((uint32_t)(mv - vLo) * (pHi - pLo) / (vHi - vLo));
    }
  }
  return 100;
}

float PowerManager::getBatteryVoltage() {
  return axp.getBattVoltage() / 1000.0f;
}

float PowerManager::getVbusVoltage() {
  return axp.getVbusVoltage() / 1000.0f;
}

// ==================== 功耗分级（TASK-A4）====================
static PowerLevel g_powerLevel = POWER_L0_NORMAL;
static unsigned long g_powerChangedMs = 0;

PowerLevel PowerManager::getPowerLevel() { return g_powerLevel; }

void PowerManager::setWakeCallback(void (*cb)()) { wakeCb = cb; }

void PowerManager::setPowerLevel(PowerLevel level) {
  if (level == g_powerLevel) return;
  PowerLevel prev = g_powerLevel;        // 记录进入前的等级，供 L0 分支判断是否从深度休眠唤醒
  g_powerLevel = level;
  g_powerChangedMs = millis();

  switch (level) {
    case POWER_L0_NORMAL:
      enableLcdPower(true);
      enableAudioPower(true);
      // 从 L2/L3 深度休眠唤醒：LCD 已重新供电，但 SH8601 控制器需重初始化才能显示
      if (prev >= POWER_L2_DEEP_SLEEP && wakeCb) wakeCb();
      Serial.println("[PMU] L0 NORMAL");
      break;

    case POWER_L1_STANDBY:
      enableAudioPower(false);
      Serial.println("[PMU] L1 STANDBY");
      break;

    case POWER_L2_DEEP_SLEEP:
      enableLcdPower(false);
      enableAudioPower(false);
      Serial.println("[PMU] L2 DEEP SLEEP");
      break;

    case POWER_L3_SHUTDOWN:
      enableLcdPower(false);
      enableAudioPower(false);
      Serial.println("[PMU] L3 SHUTDOWN - entering deep sleep with AXP2101 shutdown");
      // AXP2101 真正关机：先关闭所有输出，再触发 shutdown
      axp.disableDC1();
      axp.disableALDO1();
      axp.disableALDO2();
      axp.disableALDO3();
      axp.disableALDO4();
      axp.shutdown();  // 触发 PMU 关机，切断所有供电
      // 如果 setShutdown 不立即生效，ESP32 侧进入深度睡眠
      esp_deep_sleep_start();
      break;
  }
}

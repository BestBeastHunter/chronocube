#include "i2c_bsp.h"
#include "config.h"

bool I2CBus::begin(int sda, int scl, uint32_t freq) {
  // P2: 上电/复位后若从设备异常拉低 SDA 锁死总线，先手动翻转 SCL 9 个周期迫使其释放 SDA
  pinMode(scl, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(scl, HIGH); delayMicroseconds(5);
    digitalWrite(scl, LOW);  delayMicroseconds(5);
  }
  // P1: 新版 Arduino-ESP32 Wire.begin 返回 bool，验证总线是否正常启动
  bool ok = Wire.begin(sda, scl, freq);
  if (!ok) {
    Serial.println("[I2C] Wire.begin failed — bus may be unavailable");
  }
  Wire.flush();
  // P1: probe AXP2101 PMU (0x34) 确认 I2C 物理链路可用
  Serial.printf("[I2C] AXP2101 PMU probe (0x34): %s\n",
                probe(0x34) ? "found" : "NOT FOUND");
  return ok;
}

int I2CBus::writeReg(uint8_t addr, uint8_t reg, const uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) Wire.write(buf[i]);
  return Wire.endTransmission();  // 0 = OK
}

int I2CBus::writeReg8(uint8_t addr, uint8_t reg, uint8_t value) {
  return writeReg(addr, reg, &value, 1);
}

int I2CBus::readReg(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  int rc = Wire.endTransmission(false);  // 不发 STOP，留住总线
  if (rc != 0) return rc;

  uint8_t got = Wire.requestFrom(addr, (int)len, (int)true);
  if (got != len) return -1;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return 0;
}

int I2CBus::readReg8(uint8_t addr, uint8_t reg, uint8_t *value) {
  return readReg(addr, reg, value, 1);
}

int I2CBus::readRegAddr(uint8_t addr, const uint8_t *reg, uint8_t regLen,
                         uint8_t *buf, uint8_t bufLen) {
  Wire.beginTransmission(addr);
  Wire.write(reg, regLen);
  int rc = Wire.endTransmission(false);  /* 不发 STOP，留住总线做 repeated start */
  if (rc != 0) return rc;
  uint8_t got = Wire.requestFrom(addr, (int)bufLen, (int)true);
  if (got != bufLen) return -1;
  for (uint8_t i = 0; i < bufLen; i++) buf[i] = Wire.read();
  return 0;
}

bool I2CBus::probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void I2CBus::scan(Stream &out) {
  out.println("[I2C] scan begin...");
  uint8_t cnt = 0;
  for (uint8_t addr = 0x03; addr < 0x78; addr++) {
    if (probe(addr)) {
      out.printf("  found 0x%02X\n", addr);
      cnt++;
    }
  }
  out.printf("[I2C] done, %d device(s)\n", cnt);
}

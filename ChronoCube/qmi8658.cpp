#include "qmi8658.h"
#include "i2c_bsp.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "QMI8658";

// 内部寄存器读写（基于 I2CBus，Wire.h 封装）
static esp_err_t qmi_write(qmi8658_dev_t *dev, uint8_t reg, uint8_t value) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  return I2CBus::writeReg8(dev->i2c_addr, reg, value) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t qmi_read(qmi8658_dev_t *dev, uint8_t reg, uint8_t *buf, uint8_t len) {
  if (!dev || !buf || len == 0) return ESP_ERR_INVALID_ARG;
  return I2CBus::readReg(dev->i2c_addr, reg, buf, len) == 0 ? ESP_OK : ESP_FAIL;
}

// 对外接口仍保留原签名，内部走 qmi_write/qmi_read
esp_err_t qmi8658_write_register(qmi8658_dev_t *dev, uint8_t reg, uint8_t value) {
  return qmi_write(dev, reg, value);
}

esp_err_t qmi8658_read_register(qmi8658_dev_t *dev, uint8_t reg, uint8_t *buffer, uint8_t length) {
  return qmi_read(dev, reg, buffer, length);
}

esp_err_t qmi8658_init(qmi8658_dev_t *dev, uint8_t i2c_addr) {
  if (!dev) return ESP_ERR_INVALID_ARG;

  dev->i2c_addr = i2c_addr;
  dev->accel_lsb_div = 4096;
  dev->gyro_lsb_div = 64;
  dev->accel_unit_mps2 = false;
  dev->gyro_unit_rads = false;
  dev->display_precision = 6;
  dev->timestamp = 0;
  dev->last_raw_ts = 0;
  dev->ts_overflow_count = 0;

  // 总线探测一下
  if (!I2CBus::probe(i2c_addr)) {
    ESP_LOGE(TAG, "QMI8658 not found at 0x%02X", i2c_addr);
    return ESP_ERR_NOT_FOUND;
  }

  uint8_t who_am_i = 0;
  esp_err_t ret = qmi8658_get_who_am_i(dev, &who_am_i);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read WHO_AM_I register");
    return ret;
  }
  if (who_am_i != 0x05) {
    ESP_LOGE(TAG, "Invalid WHO_AM_I value: 0x%02X, expected 0x05", who_am_i);
    return ESP_ERR_NOT_FOUND;
  }

  // 软件复位
  ret = qmi_write(dev, QMI8658_CTRL1, 0x60);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize sensor");
    return ret;
  }
  vTaskDelay(pdMS_TO_TICKS(20));

  // 配量程/ODR
  ret = qmi8658_set_accel_range(dev, QMI8658_ACCEL_RANGE_8G);
  if (ret != ESP_OK) return ret;
  ret = qmi8658_set_accel_odr(dev, QMI8658_ACCEL_ODR_1000HZ);
  if (ret != ESP_OK) return ret;
  ret = qmi8658_set_gyro_range(dev, QMI8658_GYRO_RANGE_512DPS);
  if (ret != ESP_OK) return ret;
  ret = qmi8658_set_gyro_odr(dev, QMI8658_GYRO_ODR_1000HZ);
  if (ret != ESP_OK) return ret;

  // 同时开加速度计和陀螺仪（陀螺仪辅助姿态判断 TASK-007）
  ret = qmi8658_enable_sensors(dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);

  ESP_LOGI(TAG, "QMI8658 initialized successfully");
  return ret;
}

esp_err_t qmi8658_set_accel_range(qmi8658_dev_t *dev, qmi8658_accel_range_t range) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  switch (range) {
    case QMI8658_ACCEL_RANGE_2G:  dev->accel_lsb_div = 16384; break;
    case QMI8658_ACCEL_RANGE_4G:  dev->accel_lsb_div = 8192;  break;
    case QMI8658_ACCEL_RANGE_8G:  dev->accel_lsb_div = 4096;  break;
    case QMI8658_ACCEL_RANGE_16G: dev->accel_lsb_div = 2048;  break;
    default: return ESP_ERR_INVALID_ARG;
  }
  return qmi_write(dev, QMI8658_CTRL2, (range << 4) | 0x03);
}

esp_err_t qmi8658_set_accel_odr(qmi8658_dev_t *dev, qmi8658_accel_odr_t odr) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  uint8_t cur = 0;
  esp_err_t ret = qmi_read(dev, QMI8658_CTRL2, &cur, 1);
  if (ret != ESP_OK) return ret;
  return qmi_write(dev, QMI8658_CTRL2, (cur & 0xF0) | odr);
}

esp_err_t qmi8658_set_gyro_range(qmi8658_dev_t *dev, qmi8658_gyro_range_t range) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  switch (range) {
    case QMI8658_GYRO_RANGE_32DPS:   dev->gyro_lsb_div = 1024; break;
    case QMI8658_GYRO_RANGE_64DPS:   dev->gyro_lsb_div = 512;  break;
    case QMI8658_GYRO_RANGE_128DPS:  dev->gyro_lsb_div = 256;  break;
    case QMI8658_GYRO_RANGE_256DPS:  dev->gyro_lsb_div = 128;  break;
    case QMI8658_GYRO_RANGE_512DPS:  dev->gyro_lsb_div = 64;   break;
    case QMI8658_GYRO_RANGE_1024DPS: dev->gyro_lsb_div = 32;   break;
    case QMI8658_GYRO_RANGE_2048DPS: dev->gyro_lsb_div = 16;   break;
    case QMI8658_GYRO_RANGE_4096DPS: dev->gyro_lsb_div = 8;    break;
    default: return ESP_ERR_INVALID_ARG;
  }
  return qmi_write(dev, QMI8658_CTRL3, (range << 4) | 0x03);
}

esp_err_t qmi8658_set_gyro_odr(qmi8658_dev_t *dev, qmi8658_gyro_odr_t odr) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  uint8_t cur = 0;
  esp_err_t ret = qmi_read(dev, QMI8658_CTRL3, &cur, 1);
  if (ret != ESP_OK) return ret;
  return qmi_write(dev, QMI8658_CTRL3, (cur & 0xF0) | odr);
}

esp_err_t qmi8658_enable_accel(qmi8658_dev_t *dev, bool enable) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  uint8_t cur = 0;
  esp_err_t ret = qmi_read(dev, QMI8658_CTRL7, &cur, 1);
  if (ret != ESP_OK) return ret;
  if (enable) cur |= QMI8658_ENABLE_ACCEL; else cur &= ~QMI8658_ENABLE_ACCEL;
  return qmi_write(dev, QMI8658_CTRL7, cur);
}

esp_err_t qmi8658_enable_gyro(qmi8658_dev_t *dev, bool enable) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  uint8_t cur = 0;
  esp_err_t ret = qmi_read(dev, QMI8658_CTRL7, &cur, 1);
  if (ret != ESP_OK) return ret;
  if (enable) cur |= QMI8658_ENABLE_GYRO; else cur &= ~QMI8658_ENABLE_GYRO;
  return qmi_write(dev, QMI8658_CTRL7, cur);
}

esp_err_t qmi8658_enable_sensors(qmi8658_dev_t *dev, uint8_t enable_flags) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  return qmi_write(dev, QMI8658_CTRL7, enable_flags & 0x0F);
}

esp_err_t qmi8658_read_accel(qmi8658_dev_t *dev, float *x, float *y, float *z) {
  if (!dev || !x || !y || !z) return ESP_ERR_INVALID_ARG;
  uint8_t buf[6];
  esp_err_t ret = qmi_read(dev, QMI8658_AX_L, buf, 6);
  if (ret != ESP_OK) return ret;

  int16_t raw_x = (int16_t)((buf[1] << 8) | buf[0]);
  int16_t raw_y = (int16_t)((buf[3] << 8) | buf[2]);
  int16_t raw_z = (int16_t)((buf[5] << 8) | buf[4]);

  if (dev->accel_unit_mps2) {
    *x = (raw_x * ONE_G) / dev->accel_lsb_div;
    *y = (raw_y * ONE_G) / dev->accel_lsb_div;
    *z = (raw_z * ONE_G) / dev->accel_lsb_div;
  } else {
    *x = (raw_x * 1000.0f) / dev->accel_lsb_div;
    *y = (raw_y * 1000.0f) / dev->accel_lsb_div;
    *z = (raw_z * 1000.0f) / dev->accel_lsb_div;
  }
  return ESP_OK;
}

esp_err_t qmi8658_read_gyro(qmi8658_dev_t *dev, float *x, float *y, float *z) {
  if (!dev || !x || !y || !z) return ESP_ERR_INVALID_ARG;
  uint8_t buf[6];
  esp_err_t ret = qmi_read(dev, QMI8658_GX_L, buf, 6);
  if (ret != ESP_OK) return ret;

  int16_t raw_x = (int16_t)((buf[1] << 8) | buf[0]);
  int16_t raw_y = (int16_t)((buf[3] << 8) | buf[2]);
  int16_t raw_z = (int16_t)((buf[5] << 8) | buf[4]);

  if (dev->gyro_unit_rads) {
    *x = (raw_x * M_PI / 180.0f) / dev->gyro_lsb_div;
    *y = (raw_y * M_PI / 180.0f) / dev->gyro_lsb_div;
    *z = (raw_z * M_PI / 180.0f) / dev->gyro_lsb_div;
  } else {
    *x = (float)raw_x / dev->gyro_lsb_div;
    *y = (float)raw_y / dev->gyro_lsb_div;
    *z = (float)raw_z / dev->gyro_lsb_div;
  }
  return ESP_OK;
}

esp_err_t qmi8658_read_temp(qmi8658_dev_t *dev, float *temperature) {
  if (!dev || !temperature) return ESP_ERR_INVALID_ARG;
  uint8_t buf[2];
  esp_err_t ret = qmi_read(dev, QMI8658_TEMP_L, buf, 2);
  if (ret != ESP_OK) return ret;
  int16_t raw = (int16_t)((buf[1] << 8) | buf[0]);
  *temperature = (float)raw / 256.0f;
  return ESP_OK;
}

esp_err_t qmi8658_read_sensor_data(qmi8658_dev_t *dev, qmi8658_data_t *data) {
  if (!dev || !data) return ESP_ERR_INVALID_ARG;

  uint8_t ts[3];
  esp_err_t ret = qmi_read(dev, QMI8658_TIMESTAMP_L, ts, 3);
  if (ret == ESP_OK) {
    uint32_t timestamp = ((uint32_t)ts[2] << 16) | ((uint32_t)ts[1] << 8) | ts[0];

    if (timestamp < dev->last_raw_ts) {
      dev->ts_overflow_count++;
    }
    dev->last_raw_ts = timestamp;

    dev->timestamp = (uint32_t)dev->ts_overflow_count * 0x1000000 + timestamp;
    data->timestamp = dev->timestamp;
  }

  uint8_t b[12];
  ret = qmi_read(dev, QMI8658_AX_L, b, 12);
  if (ret != ESP_OK) return ret;

  int16_t ax = (int16_t)((b[1] << 8) | b[0]);
  int16_t ay = (int16_t)((b[3] << 8) | b[2]);
  int16_t az = (int16_t)((b[5] << 8) | b[4]);
  int16_t gx = (int16_t)((b[7] << 8) | b[6]);
  int16_t gy = (int16_t)((b[9] << 8) | b[8]);
  int16_t gz = (int16_t)((b[11] << 8) | b[10]);

  if (dev->accel_unit_mps2) {
    data->accelX = (ax * ONE_G) / dev->accel_lsb_div;
    data->accelY = (ay * ONE_G) / dev->accel_lsb_div;
    data->accelZ = (az * ONE_G) / dev->accel_lsb_div;
  } else {
    data->accelX = (ax * 1000.0f) / dev->accel_lsb_div;
    data->accelY = (ay * 1000.0f) / dev->accel_lsb_div;
    data->accelZ = (az * 1000.0f) / dev->accel_lsb_div;
  }
  if (dev->gyro_unit_rads) {
    data->gyroX = (gx * M_PI / 180.0f) / dev->gyro_lsb_div;
    data->gyroY = (gy * M_PI / 180.0f) / dev->gyro_lsb_div;
    data->gyroZ = (gz * M_PI / 180.0f) / dev->gyro_lsb_div;
  } else {
    data->gyroX = (float)gx / dev->gyro_lsb_div;
    data->gyroY = (float)gy / dev->gyro_lsb_div;
    data->gyroZ = (float)gz / dev->gyro_lsb_div;
  }
  return qmi8658_read_temp(dev, &data->temperature);
}

esp_err_t qmi8658_is_data_ready(qmi8658_dev_t *dev, bool *ready) {
  if (!dev || !ready) return ESP_ERR_INVALID_ARG;
  uint8_t s = 0;
  esp_err_t ret = qmi_read(dev, QMI8658_STATUS0, &s, 1);
  if (ret != ESP_OK) return ret;
  *ready = (s & 0x03) != 0;
  return ESP_OK;
}

esp_err_t qmi8658_get_who_am_i(qmi8658_dev_t *dev, uint8_t *who_am_i) {
  if (!dev || !who_am_i) return ESP_ERR_INVALID_ARG;
  return qmi_read(dev, QMI8658_WHO_AM_I, who_am_i, 1);
}

esp_err_t qmi8658_reset(qmi8658_dev_t *dev) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  return qmi_write(dev, QMI8658_CTRL1, 0x60);  // bit6=Soft Reset + bit5=GPAI
}

void qmi8658_set_accel_unit_mps2(qmi8658_dev_t *dev, bool use_mps2) { if (dev) dev->accel_unit_mps2 = use_mps2; }
void qmi8658_set_accel_unit_mg(qmi8658_dev_t *dev, bool use_mg)    { if (dev) dev->accel_unit_mps2 = !use_mg; }
void qmi8658_set_gyro_unit_rads(qmi8658_dev_t *dev, bool use_rads) { if (dev) dev->gyro_unit_rads = use_rads; }
void qmi8658_set_gyro_unit_dps(qmi8658_dev_t *dev, bool use_dps)   { if (dev) dev->gyro_unit_rads = !use_dps; }

void qmi8658_set_display_precision(qmi8658_dev_t *dev, int decimals) {
  if (dev && decimals >= 0 && decimals <= 10) dev->display_precision = decimals;
}
void qmi8658_set_display_precision_enum(qmi8658_dev_t *dev, qmi8658_precision_t p) {
  if (dev) dev->display_precision = (int)p;
}
int  qmi8658_get_display_precision(qmi8658_dev_t *dev) { return dev ? dev->display_precision : 0; }
bool qmi8658_is_accel_unit_mps2(qmi8658_dev_t *dev)    { return dev ? dev->accel_unit_mps2 : false; }
bool qmi8658_is_accel_unit_mg(qmi8658_dev_t *dev)       { return dev ? !dev->accel_unit_mps2 : false; }
bool qmi8658_is_gyro_unit_rads(qmi8658_dev_t *dev)      { return dev ? dev->gyro_unit_rads : false; }
bool qmi8658_is_gyro_unit_dps(qmi8658_dev_t *dev)       { return dev ? !dev->gyro_unit_rads : false; }

esp_err_t qmi8658_enable_wake_on_motion(qmi8658_dev_t *dev, uint8_t threshold) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  esp_err_t ret = qmi8658_enable_sensors(dev, QMI8658_DISABLE_ALL);
  if (ret != ESP_OK) return ret;
  ret = qmi8658_set_accel_range(dev, QMI8658_ACCEL_RANGE_2G);
  if (ret != ESP_OK) return ret;
  ret = qmi8658_set_accel_odr(dev, QMI8658_ACCEL_ODR_LOWPOWER_21HZ);
  if (ret != ESP_OK) return ret;
  ret = qmi_write(dev, 0x0B, threshold);
  if (ret != ESP_OK) return ret;
  ret = qmi_write(dev, 0x0C, 0x00);
  if (ret != ESP_OK) return ret;
  return qmi8658_enable_sensors(dev, QMI8658_ENABLE_ACCEL);
}

esp_err_t qmi8658_disable_wake_on_motion(qmi8658_dev_t *dev) {
  if (!dev) return ESP_ERR_INVALID_ARG;
  esp_err_t ret = qmi8658_enable_sensors(dev, QMI8658_DISABLE_ALL);
  if (ret != ESP_OK) return ret;
  return qmi_write(dev, 0x0B, 0x00);
}

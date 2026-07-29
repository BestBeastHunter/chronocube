#include "storage.h"
#include "config.h"
#include "i2c_bsp.h"
#include "spi_bus_lock.h"

#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>

StorageManager storage;

static bool g_ready = false;
static sdmmc_card_t *g_card = NULL;

// ==================== PCF85063 RTC 驱动 ====================
// 注：微雪 ESP32-C6 AMOLED 开发板板载 RTC 为 PCF85063，地址 0x51
// PCF85063 与 PCF8563 寄存器布局不同：
// - PCF8563: 秒@0x02，直接开始
// - PCF85063: 秒@0x04，中间多 OFFSET(0x02) 和 RAM(0x03)
#define RTC_ADDR        I2C_ADDR_PCF85063  // 0x51
#define RTC_REG_CTRL1   0x00
#define RTC_REG_CTRL2   0x01
#define RTC_REG_OFFSET  0x02
#define RTC_REG_RAM     0x03
#define RTC_REG_SECONDS 0x04
#define RTC_REG_MINUTES 0x05
#define RTC_REG_HOURS   0x06
#define RTC_REG_DAYS    0x07
#define RTC_REG_WEEKDAY 0x08
#define RTC_REG_MONTHS  0x09
#define RTC_REG_YEARS   0x0A
// PCF85063 标志位：STOP=bit5, VL(电压低)=bit7
#define RTC_CTRL1_STOP_BIT   0x20
#define RTC_SEC_VL_BIT       0x80

struct RtcTime {
  uint16_t year;   // 2000+
  uint8_t month;   // 1-12
  uint8_t day;     // 1-31
  uint8_t hour;    // 0-23
  uint8_t minute;  // 0-59
  uint8_t second;  // 0-59
  bool valid;
};

static uint8_t bcdToDec(uint8_t bcd) {
  return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static bool getRtcTime(RtcTime &t) {
  t.valid = false;

  if (!I2CBus::probe(RTC_ADDR)) return false;

  uint8_t ctrl1 = 0;
  if (I2CBus::readReg8(RTC_ADDR, RTC_REG_CTRL1, &ctrl1) != 0) return false;

  // 清除 STOP 位让时钟运行（PCF85063: STOP=bit5）
  if (ctrl1 & RTC_CTRL1_STOP_BIT) {
    uint8_t val = ctrl1 & ~RTC_CTRL1_STOP_BIT;
    I2CBus::writeReg8(RTC_ADDR, RTC_REG_CTRL1, val);
  }

  uint8_t buf[7] = {0};
  if (I2CBus::readReg(RTC_ADDR, RTC_REG_SECONDS, buf, 7) != 0) return false;

  // VL 位（bit7）=1 表示电压过低，时间不可靠
  if (buf[0] & RTC_SEC_VL_BIT) {
    Serial.println("[RTC] WARNING: VL flag set (voltage low), time invalid");
    t.valid = false;
    return false;
  }

  t.second = bcdToDec(buf[0] & 0x7F);
  t.minute = bcdToDec(buf[1] & 0x7F);
  t.hour   = bcdToDec(buf[2] & 0x3F);
  t.day    = bcdToDec(buf[3] & 0x3F);
  t.month  = bcdToDec(buf[5] & 0x1F);
  t.year   = 2000 + bcdToDec(buf[6]);

  if (t.year < 2024 || t.year > 2099) return false;
  if (t.month < 1 || t.month > 12) return false;
  if (t.day < 1 || t.day > 31) return false;
  if (t.hour > 23 || t.minute > 59 || t.second > 59) return false;

  t.valid = true;
  return true;
}

static bool g_rtcAvailable = false;

static uint8_t decToBcd(uint8_t dec) {
  return ((dec / 10) << 4) | (dec % 10);
}

// 计算星期几（0=周日...6=周六）— Tomohiko Sakamoto 公式
// P1-5: 增加月份边界防护，调用方已校验但函数自身应有防御
static int dayOfWeek(int y, int m, int d) {
  if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
  static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  y -= m < 3;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// ==================== RTC 公开 API ====================

bool StorageManager::getRtcDateTime(RtcDateTime &dt) {
  RtcTime t;
  if (!g_rtcAvailable || !getRtcTime(t)) {
    dt.valid = false;
    return false;
  }
  dt.year   = t.year;
  dt.month  = t.month;
  dt.day    = t.day;
  dt.hour   = t.hour;
  dt.minute = t.minute;
  dt.second = t.second;
  dt.valid  = true;
  return true;
}

bool StorageManager::setRtcDateTime(uint16_t year, uint8_t month, uint8_t day,
                                     uint8_t hour, uint8_t minute, uint8_t second) {
  if (!I2CBus::probe(RTC_ADDR)) {
    Serial.println("[RTC] set failed: device not found");
    return false;
  }
  if (year < 2024 || year > 2099) return false;
  if (month < 1 || month > 12 || day < 1 || day > 31) return false;
  if (hour > 23 || minute > 59 || second > 59) return false;

  uint8_t wk = (uint8_t)dayOfWeek((int)year, (int)month, (int)day);

  // PCF85063A: 先 STOP 时钟 → 写时间寄存器 → 清 STOP（手册推荐时序）
  uint8_t ctrl_buf[2] = { 0x21, 0x00 };  // CTRL1=0x21(CAP_SEL=1 + STOP=1), CTRL2=0x00
  if (I2CBus::writeReg(RTC_ADDR, RTC_REG_CTRL1, ctrl_buf, 2) != 0) {
    Serial.println("[RTC] I2C write CTRL failed");
    return false;
  }

  uint8_t time_buf[7] = {
    decToBcd(second),                // seconds @0x04
    decToBcd(minute),                // minutes @0x05
    decToBcd(hour),                  // hours @0x06
    decToBcd(day),                   // days @0x07
    wk,                              // weekday @0x08 (0=Sun...6=Sat)
    decToBcd(month),                 // months @0x09
    decToBcd((uint8_t)(year - 2000)) // years @0x0A (0-99)
  };

  if (I2CBus::writeReg(RTC_ADDR, RTC_REG_SECONDS, time_buf, 7) != 0) {
    Serial.println("[RTC] I2C write time failed");
    return false;
  }

  // 清除 STOP 位，恢复时钟运行
  uint8_t ctrl_run[1] = { 0x01 };  // CTRL1=0x01(CAP_SEL=1, STOP=0)
  I2CBus::writeReg(RTC_ADDR, RTC_REG_CTRL1, ctrl_run, 1);

  RtcTime t;
  g_rtcAvailable = getRtcTime(t);

  Serial.printf("[RTC] set: %04d-%02d-%02d %02d:%02d:%02d weekday=%d\n",
                year, month, day, hour, minute, second, wk);
  return g_rtcAvailable;
}

// ==================== SD 卡存储 ====================

bool StorageManager::begin() {
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = (gpio_num_t)PIN_SD_CS;
  slot_config.host_id = (spi_host_device_t)host.slot;

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 5;
  mount_config.allocation_unit_size = 512;

  esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &g_card);
  if (ret != ESP_OK) {
    Serial.printf("[SD] mount failed: %s\n", esp_err_to_name(ret));
    g_ready = false;
    return false;
  }

  sdmmc_card_print_info(stdout, g_card);
  g_ready = true;
  Serial.println("[SD] ready");

  /* Ensure ChronoCube subdirectories exist */
  mkdir("/sdcard/ChronoCube", 0777);
  mkdir("/sdcard/ChronoCube/logs", 0777);
  mkdir("/sdcard/ChronoCube/screenshots", 0777);
  mkdir("/sdcard/ChronoCube/fonts", 0777);

  // RTC 探测：加延迟+重试，等待芯片上电稳定
  g_rtcAvailable = false;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (I2CBus::probe(RTC_ADDR)) {
      g_rtcAvailable = true;
      break;
    }
    delay(10);
  }

  if (g_rtcAvailable) {
    RtcTime rtc;
    if (getRtcTime(rtc)) {
      Serial.printf("[RTC] online: %04d-%02d-%02d %02d:%02d:%02d\n",
                    rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute, rtc.second);
    } else {
      Serial.println("[RTC] detected but time invalid (not yet set)");
    }
  } else {
    Serial.println("[RTC] not found, using fallback date logic");
  }

  return true;
}

bool StorageManager::isReady() { return g_ready; }

bool StorageManager::isRtcAvailable() const { return g_rtcAvailable; }

void StorageManager::printSDInfo() {
  if (!g_ready || !g_card) {
    Serial.println("[SD] Not ready");
    return;
  }
  Serial.println("=== SD CARD INFO ===");
  if (g_card->csd.csd_ver == 2) {
    Serial.println("  Type: SDHC/SDXC");
  } else {
    Serial.println("  Type: SDSC");
  }
  uint64_t capacity = (uint64_t)g_card->csd.capacity * g_card->csd.sector_size;
  Serial.printf("  Capacity: %llu MB\n", capacity / (1024 * 1024));
  Serial.printf("  Sector: %d bytes\n", g_card->csd.sector_size);
  Serial.printf("  Name: %s\n", g_card->cid.name);
  Serial.printf("  Mfg ID: %d\n", g_card->cid.mfg_id);
}

uint16_t StorageManager::crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 1) crc = (crc >> 1) ^ STORAGE_CRC_POLY;
      else         crc =  crc >> 1;
    }
  }
  return crc;
}

// ==================== 日期工具 ====================

// 真实月长度（含闰年 2 月），用于无 RTC 时的 fallback 日期滚动
static uint8_t daysInMonthOf(uint16_t year, uint8_t month) {
  static const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  if (month < 1 || month > 12) return 30;
  return dim[month - 1];
}

static void makePath(char *out, size_t outlen) {
  RtcTime rtc;
  if (g_rtcAvailable && getRtcTime(rtc)) {
    snprintf(out, outlen, "/sdcard/ChronoCube/logs/%02d%02d%02d.jsonl",
             (uint8_t)(rtc.year % 100), rtc.month, rtc.day);
    return;
  }

  // 回退逻辑：优先用编译时间戳作为基准日期，避免硬编码日期漂移
  static uint8_t baseY = 26;
  static uint8_t baseM = 6;
  static uint8_t baseD = 25;
  static bool initialized = false;

  if (!initialized) {
    // Parse __DATE__ ("Mmm dd yyyy") to seed fallback date from compile time
    const char *compileDate = __DATE__;  // e.g. "Jul 16 2026"
    char monStr[4] = {0};
    int cd = 0, cy = 0;
    if (sscanf(compileDate, "%3s %d %d", monStr, &cd, &cy) == 3) {
      static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
      };
      for (int i = 0; i < 12; i++) {
        if (strncmp(monStr, months[i], 3) == 0) { baseM = (uint8_t)(i + 1); break; }
      }
      baseD = (uint8_t)cd;
      baseY = (uint8_t)(cy % 100);
    }
    initialized = true;
  }

  // P3: 用真实月长度滚动日期（替代原固定 30 天/月的近似，避免跨月偏差）
  // 限制：millis() 每约 49.7 天回卷，届时日期会跳回开机日 — 此为无 RTC 电池时的已知限制
  uint32_t elapsedDays = millis() / 86400000UL;
  static uint32_t   cachedDays = 0xFFFFFFFF;
  static uint16_t    cachedY;
  static uint8_t     cachedM, cachedD;

  if (elapsedDays != cachedDays) {
    uint16_t y = (uint16_t)(2000 + baseY);
    uint8_t m = baseM;
    uint8_t d = baseD;
    for (uint32_t i = 0; i < elapsedDays; i++) {
      d++;
      uint8_t dim = daysInMonthOf(y, m);
      if (d > dim) { d = 1; m++; if (m > 12) { m = 1; y++; } }
    }
    if (d < 1) d = 1;
    if (d > 31) d = 31;
    cachedY = y; cachedM = m; cachedD = d;
    cachedDays = elapsedDays;
  }

  snprintf(out, outlen, "/sdcard/ChronoCube/logs/%02d%02d%02d.jsonl",
           (uint8_t)(cachedY % 100), cachedM, cachedD);
}

// ==================== 延迟写入缓冲（v2） ====================

char StorageManager::s_pendingBuf[4096] = {0};
int  StorageManager::s_pendingHead = 0;
int  StorageManager::s_pendingCount = 0;
bool StorageManager::s_needFlush = false;
unsigned long StorageManager::s_lastFlushMs = 0;
int  StorageManager::s_consecutiveFlushFailures = 0;

// appendEvent：只攒内存缓冲，永不获取 SPI2 锁，不阻塞主循环
bool StorageManager::appendEvent(const char *jsonNoCRLF) {
  if (!g_ready || !jsonNoCRLF) return false;

  size_t jsonLen = strlen(jsonNoCRLF);
  uint16_t crc = crc16((const uint8_t*)jsonNoCRLF, jsonLen);

  // 预估行长度：json + "|" + 4位hex + "\n" = jsonLen + 7
  int lineLen = (int)jsonLen + 7;

  // 缓冲满或空间不足：循环丢弃最旧行直到有足够空间
  // P1-5 fix: if→while 确保丢弃足够行数（原代码只丢 1 行，可能仍不够）
  if (s_pendingCount >= PENDING_MAX_LINES ||
      s_pendingHead + lineLen >= (int)sizeof(s_pendingBuf)) {
    static unsigned long lastDiscardLog = 0;
    if (millis() - lastDiscardLog > 30000) {
      lastDiscardLog = millis();
      Serial.println("[STORAGE] WARN: event buffer full, dropping oldest entries");
    }
    while ((s_pendingCount >= PENDING_MAX_LINES ||
            s_pendingHead + lineLen >= (int)sizeof(s_pendingBuf)) &&
           s_pendingHead > 0) {
      char *firstNL = (char *)memchr(s_pendingBuf, '\n', s_pendingHead);
      if (!firstNL) { s_pendingHead = 0; s_pendingCount = 0; break; }
      int consumed = (int)(firstNL - s_pendingBuf) + 1;
      memmove(s_pendingBuf, s_pendingBuf + consumed, s_pendingHead - consumed);
      s_pendingHead -= consumed;
      if (s_pendingCount > 0) s_pendingCount--;
    }
  }

  // P1-5 fix: snprintf 返回值可能 > 剩余空间（截断场景），用实际写入量推进
  int rem = (int)sizeof(s_pendingBuf) - s_pendingHead;
  int n = snprintf(s_pendingBuf + s_pendingHead, rem,
                    "%s|%04X\n", jsonNoCRLF, crc);
  if (n > 0) {
    if (n >= rem && rem > 0) {
      // 截断：实际只写入了 rem-1 个字符（snprintf 保证空终止），强制换行
      s_pendingHead += rem - 1;
      if (s_pendingHead > 0) s_pendingBuf[s_pendingHead - 1] = '\n';
    } else {
      s_pendingHead += n;
    }
    s_pendingCount++;
  }
  return true;
}

// 标记关键事件需要立即刷盘（不阻塞，只设标志）
void StorageManager::forceFlush() {
  s_needFlush = true;
}

// 在 loop 空闲时调用：将缓冲数据批量写入 SD 卡
void StorageManager::flushPendingEvents() {
  if (s_pendingCount == 0 || s_pendingHead == 0) return;

  // 检查时间间隔或强制标志
  unsigned long now = millis();
  if (!s_needFlush && (now - s_lastFlushMs) < FLUSH_INTERVAL_MS) return;

  // 获取 SPI2 锁
  if (!spi2_lock("flush")) return;

  char path[64];
  makePath(path, sizeof(path));

  FILE *f = fopen(path, "a");
  if (!f) {
    spi2_unlock("flush");
    return;
  }

  size_t written = fwrite(s_pendingBuf, 1, s_pendingHead, f);
  fclose(f);

  if (written == (size_t)s_pendingHead) {
    s_pendingHead = 0;
    s_pendingCount = 0;
    s_needFlush = false;
    s_lastFlushMs = now;
  } else {
    // S-flush-fix: flush 失败记录告警，方便排查 SD 卡问题
    Serial.printf("[STORAGE] WARN: flush only wrote %u of %u bytes\n",
                  (unsigned)written, (unsigned)s_pendingHead);
    s_consecutiveFlushFailures++;
    if (s_consecutiveFlushFailures >= 5) {
      // 连续失败过多，丢弃最旧的事件，防止缓冲区无限增长
      size_t drop = s_pendingHead / 4;
      if (drop > 0) {
        memmove(s_pendingBuf, s_pendingBuf + drop, s_pendingHead - drop);
        s_pendingHead -= drop;
        s_pendingCount = s_pendingCount > 2 ? s_pendingCount - 2 : 0;
      }
      s_consecutiveFlushFailures = 0;
    }
  }

  spi2_unlock("flush");
}

bool StorageManager::loadDailyTotal(unsigned long &totalSecOut, uint16_t &year, uint8_t &month, uint8_t &day) {
  totalSecOut = 0;
  if (!g_ready) return false;

  RtcTime rtc;
  if (g_rtcAvailable && getRtcTime(rtc)) {
    year  = rtc.year;
    month = rtc.month;
    day   = rtc.day;
  } else {
    // 与 makePath 的回退逻辑一致：使用编译日期
    const char *compileDate = __DATE__;
    char monStr[4] = {0};
    int cd = 0, cy = 0;
    if (sscanf(compileDate, "%3s %d %d", monStr, &cd, &cy) == 3) {
      static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
      };
      for (int i = 0; i < 12; i++) {
        if (strncmp(monStr, months[i], 3) == 0) { month = (uint8_t)(i + 1); break; }
      }
      day = (uint8_t)cd;
      year = (uint16_t)(cy % 100 + 2000);
    } else {
      year = 2026; month = 7; day = 20;
    }
  }

  char path[64];
  makePath(path, sizeof(path));

  if (!spi2_lock()) return false;
  FILE *f = fopen(path, "r");
  if (!f) { spi2_unlock(); return false; }

  char line[1024];  // 与 network.cpp flush 行缓冲区口径一致
  while (fgets(line, sizeof(line), f)) {
    // 去掉末尾换行
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0) continue;

    // 找到 CRC 分隔符，拆分 JSON 与 CRC
    char *pipe = strrchr(line, '|');
    if (!pipe || pipe == line) continue;

    char *crcStr = pipe + 1;
    size_t crcLen = strlen(crcStr);
    if (crcLen != 4) continue;  // CRC 必须是 4 位十六进制

    *pipe = '\0';  // 截断，line 现在只剩 JSON 部分
    size_t jsonLen = pipe - line;

    // 计算 JSON 部分的 CRC 并与尾部比对
    uint16_t expectedCrc = (uint16_t)strtoul(crcStr, NULL, 16);
    uint16_t actualCrc   = crc16((const uint8_t *)line, jsonLen);
    if (actualCrc != expectedCrc) {
      Serial.printf("[SD] CRC mismatch, skip line: %.64s\n", line);
      continue;
    }

    // CRC 通过，提取 dur 字段
    char *p = strstr(line, "\"dur\":");
    if (p) {
      p += 6;
      unsigned long v = strtoul(p, NULL, 10);
      totalSecOut += v;
    }
  }
  fclose(f);
  spi2_unlock();
  return true;
}

// ==================== 文件清理 ====================

static int daysBetween(uint8_t y1, uint8_t m1, uint8_t d1,
                       uint8_t y2, uint8_t m2, uint8_t d2) {
  // P1: proper calendar calculation via mktime (replaces y*360+m*30+d)
  struct tm t1 = {}, t2 = {};
  t1.tm_year = 2000 + (int)y1 - 1900;
  t1.tm_mon  = (int)m1 - 1;
  t1.tm_mday = (int)d1;
  t2.tm_year = 2000 + (int)y2 - 1900;
  t2.tm_mon  = (int)m2 - 1;
  t2.tm_mday = (int)d2;
  time_t epoch1 = mktime(&t1);
  time_t epoch2 = mktime(&t2);
  return (int)difftime(epoch2, epoch1) / 86400;
}

void StorageManager::cleanupOldFiles(int keepDays) {
  if (!g_ready) return;
  if (keepDays < 1) keepDays = STORAGE_KEEP_DAYS;

  uint8_t curY, curM, curD;
  RtcTime rtc;
  bool useDateBased = (g_rtcAvailable && getRtcTime(rtc));
  if (useDateBased) {
    curY = (uint8_t)(rtc.year % 100);
    curM = rtc.month;
    curD = rtc.day;
  } else {
    // 无 RTC：按文件名字典序做计数兜底（保留最近 keepDays 个文件）
    curY = curM = curD = 0;  // 仅用于编译，实际走计数分支
  }

  if (!spi2_lock()) return;
  DIR *dir = opendir("/sdcard/ChronoCube/logs");
  if (!dir) { spi2_unlock(); return; }

  // 第一遍：收集匹配的文件名到固定数组
  static const int MAX_FILES = 64;
  char names[MAX_FILES][13];  // "YYMMDD.jsonl\0"
  int count = 0;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL && count < MAX_FILES) {
    const char *name = entry->d_name;
    size_t len = strlen(name);
    if (len != 12) continue;
    if (strcmp(name + 6, ".jsonl") != 0) continue;

    int fy, fm, fd;
    if (sscanf(name, "%02d%02d%02d", &fy, &fm, &fd) != 3) continue;

    snprintf(names[count], sizeof(names[count]), "%s", name);
    count++;
  }
  closedir(dir);

  // 按文件名字典序排序（= YYMMDD 时间序）
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (strcmp(names[i], names[j]) > 0) {
        char tmp[13];
        snprintf(tmp, sizeof(tmp), "%s", names[i]);
        snprintf(names[i], sizeof(names[i]), "%s", names[j]);
        snprintf(names[j], sizeof(tmp), "%s", tmp);
      }
    }
  }

  // 第二遍：决定保留/删除
  for (int i = 0; i < count; i++) {
    int fy, fm, fd;
    sscanf(names[i], "%02d%02d%02d", &fy, &fm, &fd);

    bool shouldDelete = false;
    if (useDateBased) {
      int diff = daysBetween((uint8_t)fy, (uint8_t)fm, (uint8_t)fd,
                             curY, curM, curD);
      shouldDelete = (diff > keepDays);
    } else {
      // 无 RTC 兜底：按文件名时序列保留最近 keepDays 个文件（每个日期 1 个 .jsonl）
      shouldDelete = (i < count - keepDays);
    }

    if (shouldDelete) {
      char fullPath[64];
      snprintf(fullPath, sizeof(fullPath), "/sdcard/ChronoCube/logs/%s", names[i]);
      int ret = unlink(fullPath);
      if (ret == 0) {
        Serial.printf("[SD] cleanup: removed %s%s\n", fullPath,
                      useDateBased ? "" : " (no-RTC fallback)");
      }
    }
  }
  spi2_unlock();
}
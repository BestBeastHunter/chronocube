#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <stdint.h>
#include "config.h"

// RTC 日期时间结构（供外部读取）
struct RtcDateTime {
  uint16_t year;   // 2000+
  uint8_t month;   // 1-12
  uint8_t day;     // 1-31
  uint8_t hour;    // 0-23
  uint8_t minute;  // 0-59
  uint8_t second;  // 0-59
  bool valid;
};

// 数据存储（文档 4.6）
// SD 卡 + JSONL 追加写 + CRC16-Modbus 校验 + PCF85063 RTC
class StorageManager {
public:
  bool begin();
  bool isReady();

  // 追加写一条 JSON 字符串到当日文件，自动附加 CRC
  // 文件名格式：/sdcard/ChronoCube/logs/YYMMDD.jsonl
  // v2: 只攒内存缓冲，不阻塞主循环，由 flushPendingEvents 统一刷盘
  bool appendEvent(const char *jsonNoCRLF);

  // 标记关键事件需要立即刷盘（不阻塞，只设标志）
  void forceFlush();

  // 在 loop 空闲时调用：将缓冲数据批量写入 SD 卡
  // 应在 uiUpdate() 之后、下次 LVGL flush 之前调用
  void flushPendingEvents();

  // 启动时按日期加载累计时长
  bool loadDailyTotal(unsigned long &totalSecOut, uint16_t &year, uint8_t &month, uint8_t &day);

  // 清理 N 天前数据
  void cleanupOldFiles(int keepDays = STORAGE_KEEP_DAYS);

  // 工具
  static uint16_t crc16(const uint8_t *data, size_t len);

  // RTC 读取（PCF85063，I2C 0x51）
  // 成功返回 true 并填入 dt；RTC 不在线或数据异常返回 false
  bool getRtcDateTime(RtcDateTime &dt);

  // RTC 写入（用于首次设置时间，如 NTP/串口命令）
  bool setRtcDateTime(uint16_t year, uint8_t month, uint8_t day,
                      uint8_t hour, uint8_t minute, uint8_t second);

  // RTC 是否可用
  bool isRtcAvailable() const;

  // 打印 SD 卡信息到串口
  void printSDInfo();

private:
  // 延迟写入缓冲（v2: appendEvent 只写此处，flushPendingEvents 统一刷盘）
  static char s_pendingBuf[4096];   // 线性累积缓冲
  static int s_pendingHead;          // 写入位置
  static int s_pendingCount;         // 已攒行数
  static bool s_needFlush;           // 关键事件强制刷盘标志
  static unsigned long s_lastFlushMs;
  static int s_consecutiveFlushFailures;  // 连续刷盘失败计数
  static const int PENDING_MAX_LINES = 16;
  static const unsigned long FLUSH_INTERVAL_MS = 200;
};

extern StorageManager storage;

#endif

#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>

// 通信模块（TASK-A1）— WiFi + MQTT + 离线缓存
// 依赖：WiFi.h、PubSubClient.h（均在 .cpp 中 include，避免污染头文件）
class CubeNetwork {
public:
  // 回调类型：收到下行指令时调用
  // 参数：topic（主题）、payload（载荷）、length（载荷长度）
  typedef void (*CmdCallback)(const char *topic, const uint8_t *payload, unsigned int length);

  void begin();
  void tick();
  bool isConnected();           // WiFi + MQTT 都已连接

  // 分项查询（供调试控制台使用）
  bool isWifiConnected()  const { return mWiFiConnected; }
  bool isMqttConnected()  const { return mMqttConnected; }
  int  getWifiRssi()      const;                     // WiFi 信号强度 dBm
  void getWifiIp(char *buf, size_t bufSize) const;  // IP 字符串

  // 按需连接（USER 键触发，不自动重连）
  void requestConnect();
  void requestMqttReconnect();  // 触发 MQTT 重连（WiFi已连接时）

  // 注册下行指令回调（在 begin() 前调用）
  void setCallback(CmdCallback cb);

  // 上报一条事件 JSON（无换行符）
  // 在线 → 直接 publish QoS=MQTT_QOS_UP；离线 → 写入 SD 卡缓存
  void reportEvent(const char *jsonNoCRLF);

  // 发布设备信息（info 主题）：正常路径直接发送，回调路径缓存到 tick() 延迟发送
  void publishInfo(const char *jsonNoCRLF);

  // 离线补传控制（外部调用）
  bool hasPendingLogs();              // SD 卡有未上传的 .jsonl 文件
  bool isFlushActive() const { return mFlushActive; }
  void flushBegin();                  // 启动补传（路径 A/B 均可调用）
  void flushPause();                  // 暂停补传（设备翻起时调用）

  // 调试状态读取（供 debug_console 使用）
  bool         isFlushWaitingAck()  const { return mFlushWaitingAck; }
  int          getFlushFileCount()  const { return mFlushFileCount; }
  int          getFlushFileIdx()    const { return mFlushFileIdx; }
  int          getFlushLineSkip()   const { return mFlushLineSkip; }
  uint16_t     getFlushNextSeq()    const { return mFlushNextSeq; }
  unsigned long getFlushAckSentMs() const { return mFlushAckSentMs; }
  int          getFlushBatchMax()   const { return FLUSH_BATCH_MAX; }
  int          getFlushAckTimeoutMs() const { return FLUSH_ACK_TIMEOUT_MS; }

  // 查询是否在 MQTT 回调上下文中（防止递归 publish）
  bool isInCallback() const { return mInCallback; }

private:
  bool mWiFiConnected;
  bool mMqttConnected;
  bool mNeedConnect;               // 按需连接标志（USER 键触发）
  bool mNeedMqttReconnect;         // 按需 MQTT 重连标志（USER 键触发）
  char mDeviceId[32];
  char mReportTopic[64];
  char mCmdTopic[64];
  char mInfoTopic[64];
  char mAckTopic[64];                // ACK 接收 topic
  unsigned long mLastWifiReconnectMs;
  unsigned long mLastMqttReconnectMs;
  unsigned long mWifiStartMs;        // 连接开始时间（用于统计耗时）
  unsigned long mLastTickMs;         // tick() 节流
  bool mNtpSynced;
  bool mNtpRtcWritten;
  unsigned long mNtpSyncStartMs;
  CmdCallback mCmdCb;

  // MQTT 回调防护：禁止在回调内递归 publish（A5 修复）
  bool mInCallback;                  // 当前是否在 sMqttCallback 上下文中
  char mPendingInfoReply[256];       // 延迟发送的 info 回复缓存
  bool mHasPendingInfo;              // 是否有待发送的 info 回复

  // 离线事件批处理状态（避免 flush 阻塞主循环）
  bool   mFlushActive;
  int    mFlushFileCount;
  int    mFlushFileIdx;
  int    mFlushLineSkip;             // 当前文件已发送行数
  char   mFlushFiles[20][32];        // 文件名列表副本
  bool   mFlushWaitingAck;           // 正在等待 ACK
  unsigned long mFlushAckSentMs;     // ACK 发送时间（超时判断）
  uint16_t mFlushNextSeq;            // 下一个待发 seq（NVS 持久化）

  static const int FLUSH_BATCH_MAX = 5;  // 每 tick 最多发 5 条
  static const int FLUSH_ACK_TIMEOUT_MS = 5000;  // ACK 超时 5 秒
  static const int MQTT_AUTO_RETRY_INTERVAL_MS = 10000;  // MQTT 自动重连间隔 10s

  void connectWiFi();
  void connectMqtt();
  void buildTopics();
  void flushOfflineCache();            // 保持兼容：connectMqtt 一次性调用
  void flushOfflineCacheBegin();       // 扫描文件列表，准备批处理
  void flushOfflineCacheTick();        // 每 tick 批处理最多 FLUSH_BATCH_MAX 条

  static void sMqttCallback(char *topic, uint8_t *payload, unsigned int length);
  static CubeNetwork *sInstance;
};

extern CubeNetwork network;

#endif

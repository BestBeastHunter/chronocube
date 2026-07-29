#include "network.h"
#include "config.h"
#include "storage.h"
#include "spi_bus_lock.h"
#include "pose.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <dirent.h>
#include <string.h>
#include <Preferences.h>

// ==================== 单例 ====================
CubeNetwork *CubeNetwork::sInstance = nullptr;
CubeNetwork network;

// ==================== 内部对象 ====================
static WiFiClient   gWiFiClient;
static PubSubClient gMqttClient(gWiFiClient);

// ==================== MQTT 回调桥接 ====================
void CubeNetwork::sMqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  if (!sInstance) return;
  sInstance->mInCallback = true;  // A5: 标记进入 MQTT 回调上下文

  // ACK 消息处理
  if (sInstance->mAckTopic[0] && strcmp(topic, sInstance->mAckTopic) == 0) {
    // 解析 {"ack":"evt","seq":42,"ok":true}
    char buf[128];
    size_t copyLen = (length < sizeof(buf) - 1) ? length : sizeof(buf) - 1;
    memcpy(buf, payload, copyLen);
    buf[copyLen] = '\0';

    // 简易 JSON 解析：找 "seq": 和 "ok":
    int16_t ackSeq = -1;
    bool ackOk = false;
    const char *seqPtr = strstr(buf, "\"seq\"");
    if (seqPtr) {
      const char *colon = strchr(seqPtr, ':');
      if (colon) ackSeq = (int16_t)atoi(colon + 1);
    }
    const char *okPtr = strstr(buf, "\"ok\"");
    if (okPtr) {
      const char *colon = strchr(okPtr, ':');
      if (colon) ackOk = (strstr(colon, "true") != NULL);
    }

    if (ackOk && ackSeq >= 0) {
      // D2: ACK seq 去重，只接受匹配当前等待 seq 的 ACK（防止 broker 重投误清状态）
      uint16_t expectedSeq = (sInstance->mFlushNextSeq > 0) ? (sInstance->mFlushNextSeq - 1) : 0;
      if ((uint16_t)ackSeq == expectedSeq) {
        sInstance->mFlushWaitingAck = false;
        Serial.printf("[NET] ACK ok seq=%d\n", ackSeq);
      } else {
        Serial.printf("[NET] ACK seq mismatch: got=%d expected=%d, ignored\n", ackSeq, expectedSeq);
      }
    }
    sInstance->mInCallback = false;  // A5: 清除回调上下文标志
    return;
  }

  // 普通指令消息
  if (sInstance->mCmdCb) {
    sInstance->mCmdCb(topic, payload, length);
  }
  sInstance->mInCallback = false;  // A5: 清除回调上下文标志
}

// ==================== begin ====================
void CubeNetwork::begin() {
  sInstance = this;

  mWiFiConnected = false;
  mMqttConnected = false;
  mNeedConnect = false;
  mNeedMqttReconnect = false;
  mCmdCb = nullptr;
  mLastWifiReconnectMs = 0;
  mLastMqttReconnectMs = 0;
  mNtpSynced = false;
  mNtpRtcWritten = false;
  mNtpSyncStartMs = 0;
  mFlushActive = false;
  mFlushFileCount = 0;
  mFlushFileIdx = 0;
  mFlushLineSkip = 0;
  mFlushWaitingAck = false;
  mFlushAckSentMs = 0;
  mLastTickMs = 0;
  mInCallback = false;
  mHasPendingInfo = false;
  mPendingInfoReply[0] = '\0';

  // 从 NVS 恢复 seq（断电不归零）
  Preferences prefs;
  if (prefs.begin("net", true)) {  // 只读
    mFlushNextSeq = prefs.getUShort("seq", 0);
    prefs.end();
  } else {
    mFlushNextSeq = 0;
  }
  mDeviceId[0] = '\0';
  mReportTopic[0] = '\0';
  mCmdTopic[0] = '\0';
  mInfoTopic[0] = '\0';

  buildTopics();

  if (strlen(MQTT_HOST) > 0) {
    gMqttClient.setServer(MQTT_HOST, MQTT_PORT);
    gMqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
    gMqttClient.setCallback(sMqttCallback);
  }

  if (strlen(WIFI_SSID) > 0) {
    connectWiFi();
  } else {
    Serial.println("[NET] WiFi SSID not configured, skip WiFi init");
  }
}

// ==================== setCallback ====================
void CubeNetwork::setCallback(CmdCallback cb) {
  mCmdCb = cb;
}

// ==================== requestConnect ====================
void CubeNetwork::requestConnect() {
  if (!mWiFiConnected && strlen(WIFI_SSID) > 0) {
    mNeedConnect = true;
    Serial.println("[NET] WiFi connect requested (by user)");
  }
}

// ==================== requestMqttReconnect ====================
void CubeNetwork::requestMqttReconnect() {
  if (mWiFiConnected && !mMqttConnected && strlen(MQTT_HOST) > 0) {
    mNeedMqttReconnect = true;
    Serial.println("[NET] MQTT reconnect requested (by user)");
  }
}

// ==================== buildTopics ====================
void CubeNetwork::buildTopics() {
  strncpy(mDeviceId, DEVICE_ID_DEFAULT, sizeof(mDeviceId) - 1);
  mDeviceId[sizeof(mDeviceId) - 1] = '\0';
  snprintf(mReportTopic, sizeof(mReportTopic), MQTT_TOPIC_REPORT_FMT, mDeviceId);
  snprintf(mCmdTopic, sizeof(mCmdTopic), MQTT_TOPIC_CMD_FMT, mDeviceId);
  snprintf(mInfoTopic, sizeof(mInfoTopic), MQTT_TOPIC_INFO_FMT, mDeviceId);
  snprintf(mAckTopic, sizeof(mAckTopic), MQTT_TOPIC_ACK_FMT, mDeviceId);
}

// ==================== connectWiFi ====================
void CubeNetwork::connectWiFi() {
  Serial.printf("[NET] WiFi connecting to SSID: %s ...\n", WIFI_SSID);
  mWifiStartMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ==================== connectMqtt ====================
void CubeNetwork::connectMqtt() {
  if (!mWiFiConnected) return;
  if (strlen(MQTT_HOST) == 0) return;

  Serial.printf("[NET] MQTT connecting to %s:%d as %s ...\n",
                MQTT_HOST, MQTT_PORT, mDeviceId);

  gWiFiClient.setTimeout(150);  // 150ms 足够局域网 TCP+Mqtt 握手，缩短阻塞窗口

  bool ok;
  if (strlen(MQTT_USERNAME) > 0) {
    ok = gMqttClient.connect(mDeviceId, MQTT_USERNAME, MQTT_PASSWORD);
  } else {
    ok = gMqttClient.connect(mDeviceId);
  }

  if (ok) {
    mMqttConnected = true;
    Serial.println("[NET] MQTT connected");

    if (gMqttClient.subscribe(mCmdTopic, MQTT_QOS_DOWN)) {
      Serial.printf("[NET] MQTT subscribed: %s\n", mCmdTopic);
    } else {
      Serial.printf("[NET] MQTT subscribe failed: %s\n", mCmdTopic);
    }
    // 订阅 ACK topic
    if (gMqttClient.subscribe(mAckTopic, MQTT_QOS_DOWN)) {
      Serial.printf("[NET] MQTT subscribed: %s\n", mAckTopic);
    }

    flushOfflineCacheBegin();  // 批处理模式：启动扫描，由 tick() 逐批发送
  } else {
    int state = gMqttClient.state();
    const char *stateStr = "unknown";
    switch(state) {
      case MQTT_CONNECTION_TIMEOUT:     stateStr = "TIMEOUT(-4)"; break;
      case MQTT_CONNECTION_LOST:        stateStr = "LOST(-3)"; break;
      case MQTT_CONNECT_FAILED:         stateStr = "CONNECT_FAILED(-2)"; break;
      case MQTT_DISCONNECTED:           stateStr = "DISCONNECTED(-1)"; break;
      case MQTT_CONNECTED:              stateStr = "CONNECTED(0)"; break;
      case MQTT_CONNECT_BAD_PROTOCOL:   stateStr = "BAD_PROTOCOL(1)"; break;
      case MQTT_CONNECT_BAD_CLIENT_ID:  stateStr = "BAD_CLIENT_ID(2)"; break;
      case MQTT_CONNECT_UNAVAILABLE:    stateStr = "UNAVAILABLE(3)"; break;
      case MQTT_CONNECT_BAD_CREDENTIALS:stateStr = "BAD_CREDENTIALS(4)"; break;
      case MQTT_CONNECT_UNAUTHORIZED:   stateStr = "UNAUTHORIZED(5)"; break;
    }
    Serial.printf("[NET] MQTT connect failed, state=%d (%s)\n", state, stateStr);
    mMqttConnected = false;
  }
}

// ==================== flushOfflineCacheBegin ====================
// 扫描离线日志目录，准备批处理文件列表
void CubeNetwork::flushOfflineCacheBegin() {
  mFlushActive = false;
  mFlushFileCount = 0;
  mFlushFileIdx = 0;
  mFlushLineSkip = 0;

  if (!storage.isReady()) {
    Serial.println("[NET] flushOfflineCacheBegin: SD not ready");
    return;
  }
  if (!isConnected()) {
    Serial.println("[NET] flushOfflineCacheBegin: not connected");
    return;
  }

  const char *logDir = "/sdcard/ChronoCube/logs";
  spi2_lock();  // SPI2 总线互斥：SD 卡读取与 LCD QSPI 共享总线
  DIR *dir = opendir(logDir);
  if (!dir) {
    spi2_unlock();
    Serial.println("[NET] flushOfflineCacheBegin: no log dir");
    return;
  }

  int n = 0;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL && n < 20) {
    const char *nm = ent->d_name;
    size_t len = strlen(nm);
    if (len >= 7 && strcmp(nm + len - 6, ".jsonl") == 0) {
      snprintf(mFlushFiles[n], sizeof(mFlushFiles[n]), "%s", nm);
      n++;
    }
  }
  closedir(dir);
  spi2_unlock();

  // 冒泡排序（按文件名日期升序：旧→新）
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (strcmp(mFlushFiles[i], mFlushFiles[j]) > 0) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%s", mFlushFiles[i]);
        snprintf(mFlushFiles[i], sizeof(mFlushFiles[i]), "%s", mFlushFiles[j]);
        snprintf(mFlushFiles[j], sizeof(tmp), "%s", tmp);
      }
    }
  }

  mFlushFileCount = n;
  mFlushActive = (n > 0);
  if (mFlushActive) {
    Serial.printf("[NET] flushOfflineCacheBegin: %d file(s) queued (batch=%d/tick)\n",
                  n, FLUSH_BATCH_MAX);
  }
}

// ==================== hasPendingLogs ====================
bool CubeNetwork::hasPendingLogs() {
  if (!storage.isReady()) return false;
  DIR *dir = opendir("/sdcard/ChronoCube/logs");
  if (!dir) return false;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    const char *nm = ent->d_name;
    size_t len = strlen(nm);
    if (len >= 7 && strcmp(nm + len - 6, ".jsonl") == 0) {
      closedir(dir);
      return true;
    }
  }
  closedir(dir);
  return false;
}

// ==================== flushBegin ====================
void CubeNetwork::flushBegin() {
  if (mFlushActive) return;
  if (!isConnected()) return;
  flushOfflineCacheBegin();
}

// ==================== flushPause ====================
void CubeNetwork::flushPause() {
  if (!mFlushActive) return;
  mFlushActive = false;
  mFlushWaitingAck = false;
  Serial.println("[NET] flush paused (device flipped up)");
}

// ==================== flushOfflineCacheTick ====================
// 非阻塞状态机：每 tick 处理一条事件 + 等待 ACK
void CubeNetwork::flushOfflineCacheTick() {
  if (!mFlushActive) return;
  if (!isConnected()) {
    Serial.println("[NET] flushOfflineCacheTick: connection lost, abort");
    mFlushActive = false;
    return;
  }
  if (!storage.isReady()) {
    Serial.println("[NET] flushOfflineCacheTick: SD lost, abort");
    mFlushActive = false;
    return;
  }

  // --- 状态：等待 ACK ---
  if (mFlushWaitingAck) {
    if (millis() - mFlushAckSentMs > FLUSH_ACK_TIMEOUT_MS) {
      Serial.println("[NET] ACK timeout, pausing flush");
      mFlushActive = false;
      mFlushWaitingAck = false;
    }
    return;  // 不阻塞，下个 tick 再检查
  }

  // --- 状态：上传中 ---
  const char *logDir = "/sdcard/ChronoCube/logs";

  if (mFlushFileIdx >= mFlushFileCount) {
    Serial.printf("[NET] flushOfflineCacheTick: ALL DONE (%d files)\n", mFlushFileCount);
    mFlushActive = false;
    // 最终持久化 seq
    Preferences prefs;
    if (prefs.begin("net", false)) {
      prefs.putUShort("seq", mFlushNextSeq);
      prefs.end();
    }
  }

  char filePath[96];
  snprintf(filePath, sizeof(filePath), "%s/%s", logDir, mFlushFiles[mFlushFileIdx]);

  if (!spi2_lock()) return;  // 获取锁失败，下个 tick 重试
  FILE *f = fopen(filePath, "r");
  if (!f) {
    spi2_unlock();
    mFlushFileIdx++;
    mFlushLineSkip = 0;
    return;
  }

  // 跳过已发送行
  int skip = mFlushLineSkip;
  char line[1024];
  while (skip-- > 0 && fgets(line, sizeof(line), f)) { /* skip */ }

  // 读取一行
  if (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
    if (len > 0) {
      char *pipePos = strrchr(line, '|');
      char *jsonPart = line;
      size_t jsonLen = len;
      if (pipePos) { *pipePos = '\0'; jsonLen = pipePos - line; }

      if (jsonLen > 0) {
        // 构造带 seq 的 JSON：在原有 JSON 中插入 "seq":xxxxx
        char jsonWithSeq[1100];
        // 找到最后一个 '}' 前插入 seq
        char *lastBrace = strrchr(jsonPart, '}');
        if (lastBrace) {
          int prefixLen = lastBrace - jsonPart;
          snprintf(jsonWithSeq, sizeof(jsonWithSeq), "%.*s,\"seq\":%u}",
                   prefixLen, jsonPart, mFlushNextSeq);
        } else {
          snprintf(jsonWithSeq, sizeof(jsonWithSeq), "%s", jsonPart);
        }

        bool ok = gMqttClient.publish(mReportTopic, (const uint8_t *)jsonWithSeq, strlen(jsonWithSeq), false);
        if (ok) {
          mFlushNextSeq++;
          // 持久化 seq 到 NVS（每 16 条写一次，减少 flash 磨损）
          if (mFlushNextSeq % 16 == 0) {
            Preferences prefs;
            if (prefs.begin("net", false)) {
              prefs.putUShort("seq", mFlushNextSeq);
              prefs.end();
            }
          }
          mFlushLineSkip++;
          mFlushWaitingAck = true;
          mFlushAckSentMs = millis();
          Serial.printf("[NET] flush: seq=%u sent (%s line %d)\n",
                        mFlushNextSeq - 1, mFlushFiles[mFlushFileIdx], mFlushLineSkip);
        } else {
          Serial.println("[NET] flush: publish failed, will retry");
        }
      }
    }
    fclose(f);          // 修复 FD 泄漏：逐行发送分支此前漏关 f（仅 EOF 分支才关）
    spi2_unlock();
    return;  // 一条一条发，下个 tick 继续
  }

  // EOF：文件发完
  bool eof = feof(f);
  fclose(f);
  spi2_unlock();

  if (eof) {
    Serial.printf("[NET] flushOfflineCacheTick: %s done (%d lines)\n",
                  mFlushFiles[mFlushFileIdx], mFlushLineSkip);

    // D3: 文件全部发完且无失败，重命名为 .sent 标记已上传
    if (mFlushLineSkip > 0) {
      char oldPath[96], newPath[96];
      const char *logDir = "/sdcard/ChronoCube/logs";
      snprintf(oldPath, sizeof(oldPath), "%s/%s", logDir, mFlushFiles[mFlushFileIdx]);
      snprintf(newPath, sizeof(newPath), "%s/%s.sent", logDir, mFlushFiles[mFlushFileIdx]);
      if (spi2_lock()) {
        if (rename(oldPath, newPath) == 0) {
          Serial.printf("[NET] flush: %s -> .sent\n", mFlushFiles[mFlushFileIdx]);
        } else {
          Serial.printf("[NET] flush: rename to .sent failed for %s\n", mFlushFiles[mFlushFileIdx]);
        }
        spi2_unlock();
      }
    }

    mFlushFileIdx++;
    mFlushLineSkip = 0;
  }
}

// ==================== tick ====================
void CubeNetwork::tick() {
  unsigned long now = millis();

  // 节流：最多每 200ms 执行一次，保证操作和显示流畅
  if (now - mLastTickMs < 200) return;
  mLastTickMs = now;

  // --- WiFi 状态检查 ---
  int wifiStatus = WiFi.status();
  bool wasConnected = mWiFiConnected;
  mWiFiConnected = (wifiStatus == WL_CONNECTED);

  // WiFi 状态变化日志
  if (!mWiFiConnected && wasConnected) {
    Serial.printf("[NET] WiFi disconnected  status=%d\n", wifiStatus);
  }
#ifdef DEBUG_SERIAL
  static int s_lastWifiStatus = -1;
  if (wifiStatus != s_lastWifiStatus && !mWiFiConnected) {
    const char *statusName = "?";
    switch (wifiStatus) {
      case WL_NO_SHIELD:       statusName = "NO_SHIELD"; break;
      case WL_IDLE_STATUS:     statusName = "IDLE"; break;
      case WL_NO_SSID_AVAIL:   statusName = "NO_SSID"; break;
      case WL_SCAN_COMPLETED:  statusName = "SCAN_DONE"; break;
      case WL_CONNECTED:       statusName = "CONNECTED"; break;
      case WL_CONNECT_FAILED:  statusName = "CONNECT_FAIL"; break;
      case WL_CONNECTION_LOST: statusName = "LOST"; break;
      case WL_DISCONNECTED:    statusName = "DISCONNECTED"; break;
      default: break;
    }
    Serial.printf("[NET-DBG] WiFi status=%d (%s)\n", wifiStatus, statusName);
  }
  s_lastWifiStatus = wifiStatus;
#endif

  // WiFi 刚连上 → 触发 SNTP 校时 + 首次 MQTT 连接（仅一次）
  if (mWiFiConnected && !wasConnected && !mNtpSynced) {
    unsigned long elapsed = now - mWifiStartMs;
    char ipStr[16] = "";
    IPAddress ip = WiFi.localIP();
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    Serial.printf("[NET] WiFi connected in %lums  RSSI=%d dBm  IP=%s\n",
        elapsed, WiFi.RSSI(), ipStr);
    Serial.println("[NET] Starting SNTP sync...");
    configTime(8 * 3600, 0, "pool.ntp.org", "time.asia.apple.com");
    mNtpSynced = true;
    mNtpSyncStartMs = now;
    Serial.println("[NET] WiFi connected, attempting first MQTT connection...");
    connectMqtt();
  }

  // SNTP 校时完成 → 回写 RTC（等待 5 秒让 NTP 实际返回）
  if (mNtpSynced && !mNtpRtcWritten && (now - mNtpSyncStartMs >= 5000)) {
    time_t now_t = time(nullptr);
    if (now_t > 1000000000) {
      struct tm *tm = localtime(&now_t);
      if (tm) {
        storage.setRtcDateTime(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                               tm->tm_hour, tm->tm_min, tm->tm_sec);
        Serial.printf("[NET] RTC written from NTP: %04d-%02d-%02d %02d:%02d:%02d\n",
                      tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                      tm->tm_hour, tm->tm_min, tm->tm_sec);
        mNtpRtcWritten = true;
      }
    }
  }

  if (!mWiFiConnected && strlen(WIFI_SSID) > 0 && mNeedConnect) {
    mNeedConnect = false;
    Serial.println("[NET] WiFi connecting on user request...");
    connectWiFi();
    return;
  }

  // --- MQTT 状态维护 ---
  if (mWiFiConnected && strlen(MQTT_HOST) > 0) {
    bool wasMqtt = mMqttConnected;
    if (gMqttClient.connected()) {
      mMqttConnected = true;
      gMqttClient.loop();
    } else {
      mMqttConnected = false;
      if (mNeedMqttReconnect) {
        mNeedMqttReconnect = false;
        Serial.println("[NET] MQTT reconnecting on user request...");
        connectMqtt();
      } else if (now - mLastMqttReconnectMs >= MQTT_AUTO_RETRY_INTERVAL_MS) {
        // 自动重连：broker 恢复后 ≤10s 内自动上线（150ms timeout 不卡主循环）
        Serial.println("[NET] MQTT auto-reconnecting...");
        connectMqtt();
      }
    }
    if (mMqttConnected && !wasMqtt) {
      Serial.printf("[NET] MQTT connected to %s:%d\n", MQTT_HOST, MQTT_PORT);
    } else if (!mMqttConnected && wasMqtt) {
      Serial.println("[NET] MQTT disconnected");
    }
  }

  // 离线事件批处理回放
  if (mFlushActive) flushOfflineCacheTick();

  // A5: 延迟发送 MQTT 回调中缓存的 info 回复
  if (mHasPendingInfo && isConnected()) {
    bool ok = gMqttClient.publish(mInfoTopic, (const uint8_t *)mPendingInfoReply,
                                  strlen(mPendingInfoReply), false);
    if (ok) {
      Serial.printf("[NET] deferred info published: %.64s\n", mPendingInfoReply);
    } else {
      Serial.println("[NET] deferred info publish failed");
    }
    mHasPendingInfo = false;
    mPendingInfoReply[0] = '\0';
  }
}

// ==================== isConnected ====================
bool CubeNetwork::isConnected() {
  return mWiFiConnected && mMqttConnected;
}

int CubeNetwork::getWifiRssi() const {
  if (!mWiFiConnected) return 0;
  return WiFi.RSSI();
}

void CubeNetwork::getWifiIp(char *buf, size_t bufSize) const {
  if (!mWiFiConnected || !buf || bufSize == 0) {
    if (buf && bufSize > 0) buf[0] = '\0';
    return;
  }
  IPAddress ip = WiFi.localIP();
  snprintf(buf, bufSize, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

// ==================== reportEvent ====================
void CubeNetwork::reportEvent(const char *jsonNoCRLF) {
  if (!jsonNoCRLF || jsonNoCRLF[0] == '\0') return;

  // A5: MQTT 回调内禁止递归 publish，只写入 SD 缓存
  if (mInCallback) {
    if (!storage.appendEvent(jsonNoCRLF))
      Serial.printf("[NET] callback-context SD append FAILED, event LOST: %.64s\n", jsonNoCRLF);
    return;
  }

  if (isConnected()) {
    bool ok = gMqttClient.publish(mReportTopic, (const uint8_t *)jsonNoCRLF,
                                  strlen(jsonNoCRLF), false);
    if (!ok) {
      Serial.printf("[NET] publish failed, fallback to SD: %.64s\n", jsonNoCRLF);
      if (!storage.appendEvent(jsonNoCRLF))
        Serial.printf("[NET] SD append FAILED, event LOST: %.64s\n", jsonNoCRLF);
    }
  } else {
    if (!storage.appendEvent(jsonNoCRLF))
      Serial.printf("[NET] offline, SD append FAILED, event LOST: %.64s\n", jsonNoCRLF);
  }
}

// ==================== publishInfo ====================
void CubeNetwork::publishInfo(const char *jsonNoCRLF) {
  if (!jsonNoCRLF || jsonNoCRLF[0] == '\0') return;
  if (!isConnected()) {
    Serial.println("[NET] publishInfo skipped: not connected");
    return;
  }

  // A5: MQTT 回调内禁止递归 publish，缓存到 tick() 中延迟发送
  if (mInCallback) {
    strncpy(mPendingInfoReply, jsonNoCRLF, sizeof(mPendingInfoReply) - 1);
    mPendingInfoReply[sizeof(mPendingInfoReply) - 1] = '\0';
    mHasPendingInfo = true;
    Serial.println("[NET] publishInfo deferred (callback context)");
    return;
  }

  bool ok = gMqttClient.publish(mInfoTopic, (const uint8_t *)jsonNoCRLF,
                                strlen(jsonNoCRLF), false);
  if (!ok) {
    Serial.println("[NET] publishInfo failed");
  } else {
    Serial.printf("[NET] info published: %.64s\n", jsonNoCRLF);
  }
}

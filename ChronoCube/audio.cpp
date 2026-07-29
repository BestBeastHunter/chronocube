#include "audio.h"
#include "config.h"
#include "i2c_bsp.h"
#include "pmu.h"
#include <driver/i2s_std.h>

AudioManager audio;

// ==================== ES8311 简易驱动（I2C）====================
#define ES8311_ADDR 0x18

static bool es8311_write_reg(uint8_t reg, uint8_t val) {
  return I2CBus::writeReg8(ES8311_ADDR, reg, val) == 0;
}

static bool es8311_init() {
  Serial.println("[AUDIO] es8311_init: START");
  int errs = 0;

  // 宏：写寄存器并统计错误
  #define ES8311_WRITE(reg, val) do { \
    if (!es8311_write_reg(reg, val)) { errs++; Serial.printf("[AUDIO] WARN: es8311 reg 0x%02X write fail\n", reg); } \
  } while(0)

  // 对齐官方 es8311_open() 完整寄存器序列
  ES8311_WRITE(0x44, 0x08);
  ES8311_WRITE(0x44, 0x08);
  ES8311_WRITE(0x01, 0x30);
  ES8311_WRITE(0x02, 0x00);
  ES8311_WRITE(0x03, 0x10);
  ES8311_WRITE(0x16, 0x24);
  ES8311_WRITE(0x04, 0x10);
  ES8311_WRITE(0x05, 0x00);
  ES8311_WRITE(0x40, 0x02);
  ES8311_WRITE(0x41, 0x58);
  ES8311_WRITE(0x42, 0x03);
  ES8311_WRITE(0x43, 0x02);
  ES8311_WRITE(0x0B, 0x00);
  ES8311_WRITE(0x0C, 0x00);
  ES8311_WRITE(0x10, 0x1F);
  ES8311_WRITE(0x11, 0x7F);
  ES8311_WRITE(0x00, 0x80);
  vTaskDelay(pdMS_TO_TICKS(20));
  uint8_t reg00 = 0;
  I2CBus::readReg8(ES8311_ADDR, 0x00, &reg00);
  reg00 &= 0xBF;
  ES8311_WRITE(0x00, reg00);
  ES8311_WRITE(0x01, 0x3F);
  uint8_t reg06 = 0;
  I2CBus::readReg8(ES8311_ADDR, 0x06, &reg06);
  reg06 &= ~0x20;
  ES8311_WRITE(0x06, reg06);
  ES8311_WRITE(0x13, 0x10);
  ES8311_WRITE(0x1B, 0x0A);
  ES8311_WRITE(0x1C, 0x6A);
  ES8311_WRITE(0x44, 0x58);
  // ES8311 时钟配置：主时钟来自 I2S MCLK=2.048MHz（8kHz×256），0x02~0x08 完成内部 PLL/分频
  ES8311_WRITE(0x02, 0x00);
  ES8311_WRITE(0x03, 0x00);
  ES8311_WRITE(0x04, 0xFF);
  ES8311_WRITE(0x05, 0x00);
  ES8311_WRITE(0x06, 0x20);
  ES8311_WRITE(0x07, 0x04);
  ES8311_WRITE(0x08, 0x10);
  // es8311_start()
  ES8311_WRITE(0x00, 0x80);
  ES8311_WRITE(0x01, 0x3F);
  ES8311_WRITE(0x09, 0x8C);
  ES8311_WRITE(0x0A, 0x00);
  ES8311_WRITE(0x17, 0xBF);
  ES8311_WRITE(0x0E, 0x02);
  ES8311_WRITE(0x12, 0x00);
  ES8311_WRITE(0x14, 0x1A);
  ES8311_WRITE(0x0D, 0x01);
  ES8311_WRITE(0x15, 0x40);
  ES8311_WRITE(0x37, 0x08);
  ES8311_WRITE(0x45, 0x00);
  ES8311_WRITE(0x40, 0x02);
  ES8311_WRITE(0x41, 0x58);
  ES8311_WRITE(0x42, 0x03);
  ES8311_WRITE(0x43, 0x02);
  ES8311_WRITE(0x31, 0x0C);

  #undef ES8311_WRITE

  audio.setVolume(0x33);  // ES8311 音量默认 0x33(→20%)，走 audio.setVolume 同步 m_volumePct，显示与硬件一一对应

  Serial.printf("[AUDIO] es8311_init: DONE (errs=%d)\n", errs);
  return (errs == 0);
}

// ==================== 文件作用域 ====================
static i2s_chan_handle_t s_tx_handle = NULL;

// ==================== 8-bit 波形生成器 ====================
// 纯整数运算，无 sinf()，CPU 开销 ~30x 低于正弦波

static void gen_square(int16_t *buf, size_t n, float freqHz, float sr, float duty, int16_t amp) {
  float period = sr / freqHz;
  for (size_t i = 0; i < n; i++) {
    float phase = fmodf((float)i / period, 1.0f);
    buf[i] = (phase < duty) ? amp : -amp;
  }
}

static void gen_triangle(int16_t *buf, size_t n, float freqHz, float sr, int16_t amp) {
  float period = sr / freqHz;
  for (size_t i = 0; i < n; i++) {
    float phase = fmodf((float)i / period, 1.0f);
    buf[i] = (int16_t)(amp * (2.0f * fabsf(2.0f * phase - 1.0f) - 1.0f));
  }
}

static void gen_noise(int16_t *buf, size_t n, int16_t amp) {
  for (size_t i = 0; i < n; i++) {
    buf[i] = (int16_t)(amp * ((esp_random() & 0xFFFF) / 32768.0f - 1.0f));
  }
}

// ==================== AudioManager ====================
bool AudioManager::begin() {
  muted = false;

  if (playBuf)    { heap_caps_free(playBuf); playBuf = NULL; }
  if (silenceBuf) { heap_caps_free(silenceBuf); silenceBuf = NULL; }

  // 8-bit: kPlayBufSamples=1500, 立体声 int16 = 1500*2*2 = 6000 bytes
  playBuf = (int16_t *)heap_caps_malloc(kPlayBufSamples * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
  silenceBuf = (int16_t *)heap_caps_malloc(kSilenceSamples * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
  if (playBuf) memset(playBuf, 0, kPlayBufSamples * 2 * sizeof(int16_t));
  if (silenceBuf) memset(silenceBuf, 0, kSilenceSamples * 2 * sizeof(int16_t));

  powerManager.enableAudioPower(true);
  vTaskDelay(pdMS_TO_TICKS(100));

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  if (i2s_new_channel(&chan_cfg, &s_tx_handle, NULL) != ESP_OK) {
    Serial.println("[AUDIO] i2s_new_channel failed");
    return false;
  }

  // I2S: 8kHz, 16-bit, Philips 格式
  i2s_std_config_t std_cfg = {
    .clk_cfg = {
      .sample_rate_hz = 8000,
      .clk_src        = I2S_CLK_SRC_DEFAULT,
      .mclk_multiple  = I2S_MCLK_MULTIPLE_256,  // MCLK=2.048MHz
    },
    .slot_cfg = {
      .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
      .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
      .slot_mode      = I2S_SLOT_MODE_STEREO,
      .slot_mask      = I2S_STD_SLOT_BOTH,
      .ws_width       = I2S_DATA_BIT_WIDTH_16BIT,
      .ws_pol         = false,
      .bit_shift      = true,
      .left_align     = false,
      .big_endian     = false,
      .bit_order_lsb  = false,
    },
    .gpio_cfg = {
      .mclk = (gpio_num_t)PIN_I2S_MCLK,
      .bclk = (gpio_num_t)PIN_I2S_SCLK,
      .ws   = (gpio_num_t)PIN_I2S_LRCK,
      .dout = (gpio_num_t)PIN_I2S_DOUT,
      .din  = (gpio_num_t)PIN_I2S_DIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  Serial.println("[AUDIO] I2S: 8kHz 16-bit stereo, 8-bit synth");
  if (i2s_channel_init_std_mode(s_tx_handle, &std_cfg) != ESP_OK) {
    Serial.println("[AUDIO] i2s_channel_init_std_mode failed");
    return false;
  }

  uint8_t *preload = (uint8_t *)heap_caps_malloc(8192, MALLOC_CAP_DMA);
  if (preload) {
    memset(preload, 0, 8192);
    size_t preloaded = 0;
    i2s_channel_preload_data(s_tx_handle, preload, 8192, &preloaded);
    heap_caps_free(preload);
  }

  if (i2s_channel_enable(s_tx_handle) != ESP_OK) {
    Serial.println("[AUDIO] i2s_channel_enable failed");
    return false;
  }

  if (!I2CBus::probe(ES8311_ADDR)) {
    Serial.println("[AUDIO] ES8311 not found");
  } else {
    es8311_init();
  }

  // I2S reconfig
  if (s_tx_handle) {
    i2s_channel_disable(s_tx_handle);
    i2s_std_slot_config_t slot_cfg = {
      .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
      .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
      .slot_mode      = I2S_SLOT_MODE_STEREO,
      .slot_mask      = I2S_STD_SLOT_BOTH,
      .ws_width       = I2S_DATA_BIT_WIDTH_16BIT,
      .ws_pol         = false,
      .bit_shift      = true,
      .left_align     = false,
      .big_endian     = false,
      .bit_order_lsb  = false,
    };
    i2s_channel_reconfig_std_slot(s_tx_handle, &slot_cfg);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(8000);
    i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
    i2s_channel_enable(s_tx_handle);
  }

  vTaskDelay(pdMS_TO_TICKS(500));

  // 后台任务
  if (m_reqQueue == nullptr) {
    m_reqQueue = xQueueCreate(8, sizeof(AudioReq));
    if (m_reqQueue == nullptr) {
      Serial.println("[AUDIO] WARN: req queue failed");
    } else if (m_taskHandle == nullptr) {
      BaseType_t tr = xTaskCreate(audioTask, "audio_task", 2560, this, 2, &m_taskHandle);
      if (tr != pdPASS) {
        Serial.println("[AUDIO] WARN: audio task failed");
        vQueueDelete(m_reqQueue);
        m_reqQueue = nullptr;
      } else {
        Serial.println("[AUDIO] 8-bit audio task started");
      }
    }
  }
  return true;
}

static bool g_audioVerbose = false;

void AudioManager::setMuted(bool m) {
  bool wasMuted = muted;
  muted = m;
  uint8_t reg31 = 0;
  if (I2CBus::readReg8(ES8311_ADDR, 0x31, &reg31) == 0) {
    if (m) reg31 |= 0x60;
    else   reg31 &= ~0x60;
    es8311_write_reg(0x31, reg31);
  } else {
    reg31 = 0x0C;
    if (m) reg31 |= 0x60;
    es8311_write_reg(0x31, reg31);
  }
}
bool AudioManager::isMuted() { return muted; }

void AudioManager::end() {
  if (m_taskHandle) { vTaskDelete(m_taskHandle); m_taskHandle = nullptr; }
  if (m_reqQueue) { vQueueDelete(m_reqQueue); m_reqQueue = nullptr; }
  if (playBuf) { heap_caps_free(playBuf); playBuf = NULL; }
  if (silenceBuf) { heap_caps_free(silenceBuf); silenceBuf = NULL; }
}

void AudioManager::setVolume(uint8_t regVal) {
  es8311_write_reg(0x32, regVal);
  m_volumePct = (uint8_t)((uint32_t)regVal * 100 / 0xFF);
}

uint8_t AudioManager::getVolumePct() {
  return m_volumePct;
}

// ==================== 8-bit 波形生成（核心）====================

void AudioManager::generateWaveform(int16_t *buf, size_t n, float freqHz,
                                    float durationMs, WaveformType wave,
                                    float duty, float volume) {
  if (n > kPlayBufSamples) n = kPlayBufSamples;
  const float sr = 8000.0f;
  int16_t amp = (int16_t)(30000.0f * volume);  // 预留 headroom

  // 生成原始波形
  switch (wave) {
    case WAVE_SQUARE:
      gen_square(buf, n, freqHz, sr, duty, amp);
      break;
    case WAVE_TRIANGLE:
      gen_triangle(buf, n, freqHz, sr, amp);
      break;
    case WAVE_NOISE:
      gen_noise(buf, n, amp);
      break;
  }

  // 包络：快速 attack + 指数衰减 release
  float total = durationMs / 1000.0f;
  float atk = 0.002f;   // 2ms attack
  float rel = 0.015f;   // 15ms release
  for (size_t i = 0; i < n; i++) {
    float t = (float)i / sr;
    float env = 1.0f;
    if (t < atk) {
      env = t / atk;
    } else if (total - t < rel) {
      float x = (total - t) / rel;
      env = x * x * (3.0f - 2.0f * x);  // smoothstep
    }
    buf[i] = (int16_t)(buf[i] * env);
  }

  // 峰值统计
  int16_t mn = 32767, mx = -32768;
  for (size_t i = 0; i < n; i++) {
    if (buf[i] < mn) mn = buf[i];
    if (buf[i] > mx) mx = buf[i];
  }
  lastMin = mn;
  lastMax = mx;
  lastPeakAbs = (mn < 0 ? (int16_t)(-mn) : mn);
  if (mx > lastPeakAbs) lastPeakAbs = mx;
}

bool AudioManager::playPcm(const int16_t *pcm, size_t samples) {
  if (muted) return false;
  if (!s_tx_handle || !pcm || !playBuf) return false;
  if (samples > kPlayBufSamples) return false;
  size_t bytes = samples * 2 * sizeof(int16_t);

  size_t written = 0;
  int retries = 0;
  while (written < bytes && retries < 3) {
    size_t chunk = 0;
    esp_err_t ret = i2s_channel_write(s_tx_handle, (const uint8_t *)pcm + written,
                                      bytes - written, &chunk, pdMS_TO_TICKS(50));
    if (ret != ESP_OK || chunk == 0) { retries++; continue; }
    retries = 0;
    written += chunk;
  }
  return written == bytes;
}

void AudioManager::playNote(float freqHz, float durationMs, WaveformType wave,
                            float duty, float volume) {
  if (!playBuf) return;
  size_t samples = (size_t)(8000.0f * durationMs / 1000.0f);
  if (samples > kPlayBufSamples) samples = kPlayBufSamples;
  if (samples == 0) return;
  generateWaveform(playBuf, samples, freqHz, durationMs, wave, duty, volume);
  bool ok = playPcm(playBuf, samples);
  if (g_audioVerbose) {
    const char *w = (wave == WAVE_SQUARE) ? "SQ" : (wave == WAVE_TRIANGLE) ? "TR" : "NS";
    Serial.printf("[AUDIO] %s f=%.0fHz d=%dms n=%d peak=%d %s\n",
                  w, freqHz, (int)durationMs, (int)samples, (int)lastPeakAbs, ok ? "OK" : "FAIL");
  }
}

void AudioManager::playSequence(const float *freqsHz, const float *durationsMs,
                                const WaveformType *waves, const float *duties,
                                const float *volumes, size_t n) {
  if (muted || n == 0 || !playBuf) return;
  float volScale = (float)getVolumePct() / 100.0f;   /* 音量设置真正影响输出响度 */
  for (size_t k = 0; k < n; k++) {
    float d = duties ? duties[k] : 0.5f;
    float v = (volumes ? volumes[k] : 0.8f) * volScale;
    playNote(freqsHz[k], durationsMs[k], waves[k], d, v);
    if (silenceBuf && k < n - 1) {
      playPcm(silenceBuf, kSilenceSamples);
    }
  }
}

// ==================== 后台任务 ====================

void AudioManager::audioTask(void *arg) {
  AudioManager *self = static_cast<AudioManager *>(arg);
  AudioReq req;
  while (true) {
    if (xQueueReceive(self->m_reqQueue, &req, portMAX_DELAY) == pdTRUE) {
      if (req.isRaw) {
        self->beepSynced(req.rawFreq, req.rawDur);
        if (req.restoreMute) self->setMuted(true);
      } else {
        self->playToneSynced(req.ev);
      }
    }
  }
}

void AudioManager::beep(float freqHz, float durationMs, bool restoreMute) {
  if (muted) return;
  if (m_reqQueue) {
    AudioReq req;
    req.ev = AUDIO_KEY_TICK; req.isRaw = true;
    req.rawFreq = freqHz; req.rawDur = durationMs; req.restoreMute = restoreMute;
    if (xQueueSend(m_reqQueue, &req, 0) != pdTRUE) {}
    return;
  }
  beepSynced(freqHz, durationMs);
  if (restoreMute) setMuted(true);
}

void AudioManager::beepSynced(float freqHz, float durationMs) {
  if (muted) return;
  playNote(freqHz, durationMs, WAVE_SQUARE, 0.5f, 0.8f);
}

void AudioManager::playTone(AudioEvent ev) {
  if (muted) return;
  if (m_reqQueue) {
    AudioReq req;
    req.ev = ev; req.isRaw = false; req.rawFreq = 0; req.rawDur = 0; req.restoreMute = false;
    if (xQueueSend(m_reqQueue, &req, 0) != pdTRUE) {}
    return;
  }
  playToneSynced(ev);
}

// ==================== 8-bit 音效定义 ====================
// 短音效：100ms（按键确认类）
// 长音效：300ms（状态切换类）
// 波形：方波=积极，三角=柔和，噪声=打击

void AudioManager::playToneSynced(AudioEvent ev) {
  if (muted) return;

  // 频率常量
  const float C4=262, E4=330, G4=392, A4=440;
  const float C5=523, E5=659, G5=784, C6=1047;

  switch (ev) {

    // ==================== 短音效 (100ms) ====================

    case AUDIO_KEY_TICK: {
      // 按键确认 — 单音方波 C5
      float f[]={C5}; float d[]={100};
      WaveformType w[]={WAVE_SQUARE};
      float du[]={0.5f}; float v[]={0.8f};
      playSequence(f,d,w,du,v,1);
      break;
    }

    case AUDIO_LOCK: {
      // 锁定 — A3→G3 方波下行
      float f[]={A4/2, G4/2}; float d[]={50, 50};
      WaveformType w[]={WAVE_SQUARE, WAVE_SQUARE};
      float du[]={0.5f, 0.5f}; float v[]={0.8f, 0.8f};
      playSequence(f,d,w,du,v,2);
      break;
    }

    case AUDIO_UNLOCK: {
      // 解锁 — G4→C5→E5 方波上行
      float f[]={G4, C5, E5}; float d[]={33, 33, 34};
      WaveformType w[]={WAVE_SQUARE, WAVE_SQUARE, WAVE_SQUARE};
      float du[]={0.5f, 0.5f, 0.5f}; float v[]={0.7f, 0.8f, 0.8f};
      playSequence(f,d,w,du,v,3);
      break;
    }

    case AUDIO_POSE_CONFIRM: {
      // 情绪确认 — E5→G5 三角波双音
      float f[]={E5, G5}; float d[]={45, 55};
      WaveformType w[]={WAVE_TRIANGLE, WAVE_TRIANGLE};
      float du[]={0.5f, 0.5f}; float v[]={0.8f, 0.8f};
      playSequence(f,d,w,du,v,2);
      break;
    }

    case AUDIO_EMOTION_TIMEOUT: {
      // 情绪超时 — E5→G5 方波双音（催促感）
      float f[]={E5, G5}; float d[]={45, 55};
      WaveformType w[]={WAVE_SQUARE, WAVE_SQUARE};
      float du[]={0.5f, 0.5f}; float v[]={0.8f, 0.8f};
      playSequence(f,d,w,du,v,2);
      break;
    }

    case AUDIO_REVIEW_DUE: {
      // 复习提醒 — C5→E5 脉冲波
      float f[]={C5, E5}; float d[]={45, 55};
      WaveformType w[]={WAVE_SQUARE, WAVE_SQUARE};
      float du[]={0.25f, 0.25f}; float v[]={0.7f, 0.8f};
      playSequence(f,d,w,du,v,2);
      break;
    }

    // ==================== 长音效 (300ms) ====================

    case AUDIO_FOCUS_START: {
      // 专注开始 — C5→E5→G5 方波琶音
      float f[]={C5, E5, G5}; float d[]={90, 90, 120};
      WaveformType w[]={WAVE_SQUARE, WAVE_SQUARE, WAVE_SQUARE};
      float du[]={0.5f, 0.5f, 0.5f}; float v[]={0.7f, 0.8f, 0.8f};
      playSequence(f,d,w,du,v,3);
      break;
    }

    case AUDIO_PAUSE: {
      // 暂停 — E5→C5 方波下行
      float f[]={E5, C5}; float d[]={120, 180};
      WaveformType w[]={WAVE_SQUARE, WAVE_SQUARE};
      float du[]={0.5f, 0.5f}; float v[]={0.8f, 0.7f};
      playSequence(f,d,w,du,v,2);
      break;
    }

    case AUDIO_RESUME: {
      // 恢复专注 — C5→E5 方波上行
      float f[]={C5, E5}; float d[]={80, 200};
      WaveformType w[]={WAVE_SQUARE, WAVE_SQUARE};
      float du[]={0.5f, 0.5f}; float v[]={0.7f, 0.8f};
      playSequence(f,d,w,du,v,2);
      break;
    }

    case AUDIO_CYCLE_END: {
      // 周期完成 — C5-E5-G5→C6 方波快速上行 + 噪声鼓点
      float f1[]={C5, E5, G5, C6}; float d1[]={30, 30, 30, 110};
      WaveformType w1[]={WAVE_SQUARE, WAVE_SQUARE, WAVE_SQUARE, WAVE_SQUARE};
      float du1[]={0.5f, 0.5f, 0.5f, 0.5f}; float v1[]={0.6f, 0.7f, 0.8f, 0.8f};
      playSequence(f1,d1,w1,du1,v1,4);
      // 噪声鼓点
      if (silenceBuf) playPcm(silenceBuf, 10);  // 2.5ms gap
      float fn[]={0}; float dn[]={60};
      WaveformType wn[]={WAVE_NOISE};
      float dun[]={0.5f}; float vn[]={0.4f};
      playSequence(fn,dn,wn,dun,vn,1);
      break;
    }

    case AUDIO_REST_START: {
      // 休息开始 — G5→E5→C5 三角波下行（柔和）
      float f[]={G5, E5, C5}; float d[]={100, 100, 100};
      WaveformType w[]={WAVE_TRIANGLE, WAVE_TRIANGLE, WAVE_TRIANGLE};
      float du[]={0.5f, 0.5f, 0.5f}; float v[]={0.8f, 0.7f, 0.6f};
      playSequence(f,d,w,du,v,3);
      break;
    }

    case AUDIO_REST_END: {
      // 休息结束 — C5→E5 三角波上行（温和唤醒）
      float f[]={C5, E5}; float d[]={120, 180};
      WaveformType w[]={WAVE_TRIANGLE, WAVE_TRIANGLE};
      float du[]={0.5f, 0.5f}; float v[]={0.7f, 0.8f};
      playSequence(f,d,w,du,v,2);
      break;
    }

    case AUDIO_LOW_BATTERY: {
      // 低电量 — A4→E4→A4 方波三连
      float f[]={A4, E4, A4}; float d[]={90, 90, 90};
      WaveformType w[]={WAVE_SQUARE, WAVE_SQUARE, WAVE_SQUARE};
      float du[]={0.5f, 0.5f, 0.5f}; float v[]={0.8f, 0.8f, 0.8f};
      playSequence(f,d,w,du,v,3);
      break;
    }

    default: {
      // fallback: 单音方波
      float f[]={C5}; float d[]={100};
      WaveformType w[]={WAVE_SQUARE};
      float du[]={0.5f}; float v[]={0.8f};
      playSequence(f,d,w,du,v,1);
      break;
    }
  }
}

// ==================== 按名称播放（串口调试）====================

bool AudioManager::playSoundByName(const char *name) {
  if (!name || !name[0]) return false;
  struct SoundEntry { const char *name; AudioEvent ev; };
  static const SoundEntry table[] = {
    { "focus_start",     AUDIO_FOCUS_START },
    { "pause",           AUDIO_PAUSE },
    { "resume",          AUDIO_RESUME },
    { "cycle_end",       AUDIO_CYCLE_END },
    { "rest_start",      AUDIO_REST_START },
    { "rest_end",        AUDIO_REST_END },
    { "emotion_timeout", AUDIO_EMOTION_TIMEOUT },
    { "pose_confirm",    AUDIO_POSE_CONFIRM },
    { "emotion",         AUDIO_POSE_CONFIRM },  // 兼容旧名（pose_confirm 的别名），防止 beep emotion 行为回退
    { "low_battery",     AUDIO_LOW_BATTERY },
    { "lock",            AUDIO_LOCK },
    { "unlock",          AUDIO_UNLOCK },
    { "key_tick",        AUDIO_KEY_TICK },
    { "review_due",      AUDIO_REVIEW_DUE },
  };
  for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
    if (strcmp(name, table[i].name) == 0) {
      playTone(table[i].ev);
      return true;
    }
  }
  return false;
}

// ==================== 调试 ====================

void AudioManager::setDebugVerbose(bool v) {
  g_audioVerbose = v;
}

void AudioManager::debugHealthDump() {
  Serial.println("=== [AUDIO] HEALTH DUMP ===");
  uint8_t volReg = 0;
  I2CBus::readReg8(ES8311_ADDR, 0x32, &volReg);
  Serial.printf("  muted=%s volume=0x%02X\n", muted ? "yes" : "no", volReg);
  Serial.printf("  I2S=%s playBuf=%s kPlayBuf=%d\n",
                 s_tx_handle ? "OK" : "NULL", playBuf ? "OK" : "NULL", (int)kPlayBufSamples);
  Serial.printf("  last: peak=%d range[%d,%d]\n", (int)lastPeakAbs, (int)lastMin, (int)lastMax);
  Serial.println("=== END ===");
}

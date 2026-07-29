#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// 8-bit 波形类型（v2.1: 从正弦波迁移到复古游戏风格）
enum WaveformType {
  WAVE_SQUARE,    // 方波：明亮电子感，用于积极事件
  WAVE_TRIANGLE,  // 三角波：圆润柔和，用于休息相关
  WAVE_NOISE,     // 白噪声：沙沙声，用于打击点缀
};

// 音频反馈
// 8-bit 风格：方波/三角/噪声实时合成，无 sinf() 依赖
// CPU 开销比正弦波低 ~30 倍（纯整数运算 vs 浮点三角函数）
class AudioManager {
public:
  bool begin();
  void end();
  void setMuted(bool m);
  bool isMuted();
  void setVolume(uint8_t regVal);
  uint8_t getVolumePct();
  void playTone(AudioEvent ev);

  bool playSoundByName(const char *name);
  bool playPcm(const int16_t *pcm, size_t samples);

  void beep(float freqHz, float durationMs, bool restoreMute = false);
  void setDebugVerbose(bool v);
  void debugHealthDump();

private:
  volatile bool muted;
  // 8-bit: 最长单音 200ms@8kHz = 1600 samples, 留余量到 3000
  static constexpr size_t kPlayBufSamples = 3000;
  int16_t *playBuf;
  int16_t *silenceBuf;
  static constexpr size_t kSilenceSamples = 24;    // 3ms @ 8kHz

  int16_t lastPeakAbs = 0;
  int16_t lastMin = 0;
  int16_t lastMax = 0;
  volatile uint8_t m_volumePct = 75;   // 当前音量百分比（0-100），跨任务访问需 volatile

  // 8-bit 波形生成器（无 sinf()，纯整数运算）
  void generateWaveform(int16_t *buf, size_t n, float freqHz, float durationMs,
                        WaveformType wave, float duty, float volume);
  void playNote(float freqHz, float durationMs, WaveformType wave,
                float duty, float volume);
  void playSequence(const float *freqsHz, const float *durationsMs,
                    const WaveformType *waves, const float *duties,
                    const float *volumes, size_t n);

  struct AudioReq {
    AudioEvent ev;
    bool isRaw;
    float rawFreq;
    float rawDur;
    bool restoreMute;
  };
  QueueHandle_t m_reqQueue = nullptr;
  TaskHandle_t  m_taskHandle = nullptr;
  static void audioTask(void *arg);
  void playToneSynced(AudioEvent ev);
  void beepSynced(float freqHz, float durationMs);
};

extern AudioManager audio;

#endif

#ifndef TIMER_H
#define TIMER_H

#include <Arduino.h>
#include "config.h"

class TimerManager {
public:
  void begin();

  // 主循环每秒调用一次
  void tick();

  // ========== 状态切换（按 v5.0 状态机）==========

  // 进入某专注模式（S1/S2/S3），newWorkState 必须是 STATE_DEEP_FOCUS/LIGHT_WORK/STUDY
  // isNewCycle = true：开启新周期（结算上一周期）
  // isNewCycle = false：从暂停恢复（接续上一周期，不结算）
  void enterFocus(SystemState newWorkState, bool isNewCycle);

  // 进入暂停 S4（从 S1/S2/S3 进入，保存 previousWorkState 和当前进度）
  void enterPause();

  // 进入情绪选择 S8（专注结束时调用）
  void enterEmotionPick();

  // 进入对应休息模式（情绪选择后，按 previousWorkState 决定进入哪种休息）
  void enterRest(SystemState previousWorkState);

  // 进入待机 S0（反扣结算后）
  void enterStandby();

  // ========== 查询 ==========

  SystemState getState() { return currentState; }
  const char* stateName() { return stateName(currentState); }
  const char* stateNameEn() { return stateNameEn(currentState); }
  unsigned long getCurrentDuration() { return currentDuration; }
  unsigned long getCycleTotal();       // 当前状态的周期总时长（秒）
  float getProgressPct();              // 0.0~1.0

  // 今日统计
  unsigned long getTotalToday() { return totalToday; }
  unsigned long getEffectiveToday() { return effectiveToday; }
  unsigned long getIneffectiveToday() { return ineffectiveToday; }

  // 各模式今日累计（用于 S0 待机屏展示）
  unsigned long getModeToday(SystemState s) { return (s < 9) ? modeToday[s] : 0; }

  // 暂停相关
  SystemState getPreviousWorkState() { return previousWorkState; }
  unsigned long getPauseDuration() { return pauseDuration; }
  unsigned long getSavedWorkDuration() { return savedWorkDuration; }

  // 标志位（状态机轮询用）
  bool isCycleEndFlag() { return cycleEndFlag; }
  void clearCycleEndFlag() { cycleEndFlag = false; }

  bool isRestEndFlag() { return restEndFlag; }
  void clearRestEndFlag() { restEndFlag = false; }

  // 休息结束倒计时
  bool isRestEnded() { return restEnded; }
  uint16_t getRestEndCountdown() { return restEndCountdown; }
  void tickRestEndCountdown();

  // 0 点日清零
  void dailyReset(uint16_t year, uint8_t month, uint8_t day);

  // 配置覆盖（中枢下发）
  void setModeWorkTime(SystemState s, unsigned long sec);
  void setModeRestTime(SystemState s, unsigned long sec);

  // 设置低效专注阈值
  void setIneffectiveRatio(float ratio) { ineffectiveRatio = ratio; }

  // P1-1: 情绪选择后由 App 层调用，分类到有效/无效
  void addEffectiveToday(unsigned long s)   { effectiveToday += s; }
  void addIneffectiveToday(unsigned long s) { ineffectiveToday += s; }

  // v5.3 P0-2: 本周期是否有过暂停（完整周期判定用）
  bool getHadPause() { return hadPause; }

  // 最近一次每日重置日期
  void getLastResetDate(uint16_t &y, uint8_t &m, uint8_t &d) const {
    y = lastResetY; m = lastResetM; d = lastResetD;
  }

  // 批量应用运行时配置（config_loader 调用）
  void applyRuntimeConfig(unsigned long deepWork, unsigned long deepRest,
                          unsigned long lightWork, unsigned long lightRest,
                          unsigned long studyWork, unsigned long studyRest,
                          float ineffectiveRatioVal);

  // 调试专用：设置当前状态已流逝秒数（仅串口控制台使用）
  void debugSetElapsed(unsigned long sec) { currentDuration = sec; }

  // 暂停中途切换模式：将暂停前保存的工作时长记为低效，更新全部统计
  void commitPauseWorkAsIncomplete();

private:
  SystemState   currentState;
  unsigned long currentDuration;  // 当前状态已持续秒数
  unsigned long totalToday;       // 今日总专注（有效+低效）
  unsigned long effectiveToday;   // 今日有效专注
  unsigned long ineffectiveToday; // 今日低效专注
  unsigned long modeToday[9];     // 各状态今日累计（主要使用 S1/S2/S3）
  SystemState   previousWorkState; // S4 暂停前的工作模式
  unsigned long savedWorkDuration; // S4 暂停时保存的已专注时长
  unsigned long pauseDuration;     // 暂停时长（秒）

  bool cycleEndFlag;  // 专注周期结束标志
  bool restEndFlag;   // 休息结束标志

  bool restEnded;     // 休息是否已结束（停在提示页）
  uint16_t restEndCountdown; // 休息结束倒计时秒数
  bool hadPause;      // v5.3: 本周期是否有过暂停

  uint16_t lastResetY;
  uint8_t  lastResetM;
  uint8_t  lastResetD;

  // 各模式工作/休息时长配置
  unsigned long workTime[9];
  unsigned long restTime[9];

  // 辅助：结算当前工作周期到今日总计
  void commitWorkCycle();
  // 辅助：获取某状态名
  const char* stateName(SystemState s);
  const char* stateNameEn(SystemState s);

  float ineffectiveRatio;  // 可运行时覆盖 INEFFECTIVE_RATIO
};

extern TimerManager timerManager;

#endif // TIMER_H

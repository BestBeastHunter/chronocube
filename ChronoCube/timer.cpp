#include "timer.h"
#include <Arduino.h>

TimerManager timerManager;

void TimerManager::begin() {
  currentState       = STATE_STANDBY;
  currentDuration    = 0;
  totalToday         = 0;
  effectiveToday     = 0;
  ineffectiveToday   = 0;
  for (int i = 0; i < 9; i++) modeToday[i] = 0;
  previousWorkState  = STATE_DEEP_FOCUS;
  savedWorkDuration  = 0;
  pauseDuration      = 0;
  cycleEndFlag       = false;
  restEndFlag        = false;
  restEnded          = false;
  restEndCountdown   = 5;
  hadPause           = false;
  lastResetY = 2099; lastResetM = 1; lastResetD = 1;
  ineffectiveRatio   = INEFFECTIVE_RATIO;

  workTime[STATE_DEEP_FOCUS]  = MODE_DEEP_FOCUS_WORK;
  workTime[STATE_LIGHT_WORK]  = MODE_LIGHT_WORK_WORK;
  workTime[STATE_STUDY]       = MODE_STUDY_WORK;
  workTime[STATE_PAUSE]       = 0;
  workTime[STATE_DEEP_REST]   = 0;
  workTime[STATE_LIGHT_REST]  = 0;
  workTime[STATE_STUDY_REST]  = 0;
  workTime[STATE_STANDBY]     = 0;
  workTime[STATE_EMOTION_PICK]= 0;

  restTime[STATE_DEEP_FOCUS]  = MODE_DEEP_FOCUS_REST;
  restTime[STATE_LIGHT_WORK]  = MODE_LIGHT_WORK_REST;
  restTime[STATE_STUDY]       = MODE_STUDY_REST;
  restTime[STATE_PAUSE]       = 0;
  restTime[STATE_DEEP_REST]   = MODE_DEEP_FOCUS_REST;
  restTime[STATE_LIGHT_REST]  = MODE_LIGHT_WORK_REST;
  restTime[STATE_STUDY_REST]  = MODE_STUDY_REST;
  restTime[STATE_STANDBY]     = 0;
  restTime[STATE_EMOTION_PICK]= 0;
}

unsigned long TimerManager::getCycleTotal() {
  if (currentState == STATE_DEEP_FOCUS ||
      currentState == STATE_LIGHT_WORK ||
      currentState == STATE_STUDY) {
    return workTime[currentState];
  }
  if (currentState == STATE_DEEP_REST ||
      currentState == STATE_LIGHT_REST ||
      currentState == STATE_STUDY_REST) {
    return restTime[currentState];
  }
  return 0;
}

float TimerManager::getProgressPct() {
  unsigned long total = getCycleTotal();
  if (total == 0) return 0.0f;
  float pct = (float)currentDuration / total;
  if (pct > 1.0f) pct = 1.0f;
  return pct;
}

void TimerManager::commitWorkCycle() {
  // v5.3: commitWorkCycle 仅在中途退出时调用（反扣/模式切换）
  // → 一律记为低效，不看时长比例
  if (currentState == STATE_DEEP_FOCUS ||
      currentState == STATE_LIGHT_WORK ||
      currentState == STATE_STUDY) {
    if (currentDuration > 0) {
#ifdef DEBUG_SERIAL
      Serial.printf("[TMR] commitWorkCycle: %s dur=%lus → ineffective\n",
          stateName(currentState), (unsigned long)currentDuration);
#endif
      totalToday += currentDuration;
      modeToday[currentState] += currentDuration;
      ineffectiveToday += currentDuration;
    }
  }
}

void TimerManager::enterFocus(SystemState newWorkState, bool isNewCycle) {
  if (newWorkState != STATE_DEEP_FOCUS &&
      newWorkState != STATE_LIGHT_WORK &&
      newWorkState != STATE_STUDY) {
    return;
  }

#ifdef DEBUG_SERIAL
  Serial.printf("[TMR] enterFocus: %s  isNewCycle=%d\n",
      stateName(newWorkState), isNewCycle);
#endif

  if (isNewCycle) {
    // 新周期：先结算上一个工作周期
    commitWorkCycle();
    currentDuration = 0;
    pauseDuration = 0;
    hadPause = false;  // v5.3: 新周期重置暂停标记
  } else {
    // 从暂停恢复：接续 savedWorkDuration
    currentDuration = savedWorkDuration;
    savedWorkDuration = 0;  // 清零，避免后续误用
    // hadPause 保持 true，本周期已有暂停
  }

  currentState  = newWorkState;
  cycleEndFlag  = false;
  restEndFlag   = false;
  restEnded     = false;
}

void TimerManager::enterPause() {
#ifdef DEBUG_SERIAL
  Serial.printf("[TMR] enterPause: from %s dur=%lus\n",
      stateName(currentState), (unsigned long)currentDuration);
#endif
  if (currentState == STATE_DEEP_FOCUS ||
      currentState == STATE_LIGHT_WORK ||
      currentState == STATE_STUDY) {
    // 从工作模式进入暂停，保存进度
    previousWorkState = currentState;
    savedWorkDuration = currentDuration;
  } else if (currentState == STATE_DEEP_REST ||
             currentState == STATE_LIGHT_REST ||
             currentState == STATE_STUDY_REST) {
    // 从休息进入暂停，保存休息状态（用 previousWorkState 映射）
    if (currentState == STATE_DEEP_REST)  previousWorkState = STATE_DEEP_FOCUS;
    if (currentState == STATE_LIGHT_REST) previousWorkState = STATE_LIGHT_WORK;
    if (currentState == STATE_STUDY_REST) previousWorkState = STATE_STUDY;
    savedWorkDuration = 0;
  }
  currentState  = STATE_PAUSE;
  pauseDuration = 0;
  hadPause      = true;   // v5.3: 标记本周期有暂停
  cycleEndFlag  = false;
  restEndFlag   = false;
}

void TimerManager::enterEmotionPick() {
#ifdef DEBUG_SERIAL
  Serial.printf("[TMR] enterEmotionPick: after %s dur=%lus hadPause=%d\n",
      stateName(currentState), (unsigned long)currentDuration, hadPause);
#endif
  currentState    = STATE_EMOTION_PICK;
  currentDuration = 0;
  cycleEndFlag    = false;
  restEnded       = false;
}

void TimerManager::enterRest(SystemState prevWork) {
  // 根据上一个工作模式进入对应休息
  if (prevWork == STATE_DEEP_FOCUS)  currentState = STATE_DEEP_REST;
  else if (prevWork == STATE_LIGHT_WORK) currentState = STATE_LIGHT_REST;
  else if (prevWork == STATE_STUDY)       currentState = STATE_STUDY_REST;
  else                                     currentState = STATE_DEEP_REST;

#ifdef DEBUG_SERIAL
  unsigned long rTime = restTime[currentState];
  Serial.printf("[TMR] enterRest: %s  prevWork=%s  restTime=%lus\n",
      stateName(currentState), stateName(prevWork), rTime);
#endif

  currentDuration = 0;
  restEndFlag     = false;
  restEnded       = false;
  restEndCountdown = 5;
}

void TimerManager::enterStandby() {
#ifdef DEBUG_SERIAL
  Serial.printf("[TMR] enterStandby: from %s dur=%lus\n",
      stateName(currentState), (unsigned long)currentDuration);
#endif
  // 当前是工作模式 → 直接结算
  if (currentState == STATE_DEEP_FOCUS ||
      currentState == STATE_LIGHT_WORK ||
      currentState == STATE_STUDY) {
    commitWorkCycle();
  }
  // 当前是暂停 → 结算暂停前保存的工作时长
  // v5.3: 暂停中途退出 → 一律低效
  else if (currentState == STATE_PAUSE) {
    if (savedWorkDuration > 0 &&
        (previousWorkState == STATE_DEEP_FOCUS ||
         previousWorkState == STATE_LIGHT_WORK ||
         previousWorkState == STATE_STUDY)) {
      totalToday += savedWorkDuration;
      modeToday[previousWorkState] += savedWorkDuration;
      ineffectiveToday += savedWorkDuration;
    }
  }
  currentState    = STATE_STANDBY;
  currentDuration = 0;
  pauseDuration   = 0;
  cycleEndFlag    = false;
  restEndFlag     = false;
  restEnded       = false;
  restEndCountdown = 5;
}

void TimerManager::tick() {
  // 待机/情绪选择 不计时；暂停在 enterPause 中已保存暂停时长
  if (currentState == STATE_STANDBY ||
      currentState == STATE_EMOTION_PICK) {
    return;
  }

  if (currentState == STATE_PAUSE) {
    pauseDuration++;
    return;
  }

  // 休息结束后停在提示页，不继续计时
  if (restEnded) return;

  currentDuration++;

  unsigned long total = getCycleTotal();
  if (total > 0 && currentDuration >= total) {
    if (currentState == STATE_DEEP_FOCUS ||
        currentState == STATE_LIGHT_WORK ||
        currentState == STATE_STUDY) {
      // 工作周期自然结束：暂存数据，等情绪选择后再分类有效/无效
      // P1-1 fix: 不在此处直接加 effectiveToday，由 onEmotionConfirm 分类
#ifdef DEBUG_SERIAL
      Serial.printf("[TMR] tick: focus end  %s dur=%lus/%lus\n",
          stateName(currentState), (unsigned long)currentDuration, total);
#endif
      cycleEndFlag = true;
      totalToday     += currentDuration;
      modeToday[currentState] += currentDuration;
      previousWorkState = currentState;
      savedWorkDuration = currentDuration;
      currentState = STATE_EMOTION_PICK;
      currentDuration = 0;
    } else if (currentState == STATE_DEEP_REST ||
               currentState == STATE_LIGHT_REST ||
               currentState == STATE_STUDY_REST) {
      // 休息结束：停在提示页
#ifdef DEBUG_SERIAL
      Serial.printf("[TMR] tick: rest end  %s dur=%lus/%lus\n",
          stateName(currentState), (unsigned long)currentDuration, total);
#endif
      restEnded   = true;
      restEndFlag = true;
    }
  }
}

void TimerManager::tickRestEndCountdown() {
  if (!restEnded) return;
  if (restEndCountdown > 0) restEndCountdown--;
}

void TimerManager::dailyReset(uint16_t year, uint8_t month, uint8_t day) {
  // T2-fix: 最小日期校验，防止 RTC 未初始化时的异常值触发错误重置
  if (year < 2025 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) return;
  if (year == lastResetY && month == lastResetM && day == lastResetD) return;
#ifdef DEBUG_SERIAL
  Serial.printf("[TMR] dailyReset: %d/%02d/%02d\n", year, month, day);
#endif
  totalToday       = 0;
  effectiveToday   = 0;
  ineffectiveToday = 0;
  for (int i = 0; i < 9; i++) modeToday[i] = 0;
  lastResetY = year; lastResetM = month; lastResetD = day;
}

void TimerManager::setModeWorkTime(SystemState s, unsigned long sec) {
  // P1-5: 拒绝 0 值避免计时器永不结束（workTime=0 → total=0 → 永不触发周期结束）
  if (s < 9 && sec > 0) workTime[s] = sec;
}

void TimerManager::setModeRestTime(SystemState s, unsigned long sec) {
  if (s < 9 && sec > 0) restTime[s] = sec;  // P1-5: 同样守卫 0 值
}

const char* TimerManager::stateName(SystemState s) {
  switch (s) {
    case STATE_STANDBY:       return "待机";
    case STATE_DEEP_FOCUS:    return "深度专注";
    case STATE_LIGHT_WORK:    return "轻量事务";
    case STATE_STUDY:         return "学习成长";
    case STATE_PAUSE:         return "临时暂停";
    case STATE_DEEP_REST:     return "深度休息";
    case STATE_LIGHT_REST:    return "轻量休息";
    case STATE_STUDY_REST:    return "学习休息";
    case STATE_EMOTION_PICK:  return "情绪选择";
  }
  return "?";
}

const char* TimerManager::stateNameEn(SystemState s) {
  switch (s) {
    case STATE_STANDBY:       return "standby";
    case STATE_DEEP_FOCUS:    return "deep_focus";
    case STATE_LIGHT_WORK:    return "light_work";
    case STATE_STUDY:         return "study";
    case STATE_PAUSE:         return "pause";
    case STATE_DEEP_REST:     return "deep_rest";
    case STATE_LIGHT_REST:    return "light_rest";
    case STATE_STUDY_REST:    return "study_rest";
    case STATE_EMOTION_PICK:  return "emotion";
  }
  return "?";
}

void TimerManager::commitPauseWorkAsIncomplete() {
  if (currentState == STATE_PAUSE &&
      savedWorkDuration > 0 &&
      (previousWorkState == STATE_DEEP_FOCUS ||
       previousWorkState == STATE_LIGHT_WORK ||
       previousWorkState == STATE_STUDY)) {
    totalToday += savedWorkDuration;
    modeToday[previousWorkState] += savedWorkDuration;
    ineffectiveToday += savedWorkDuration;
    savedWorkDuration = 0;
  }
}

void TimerManager::applyRuntimeConfig(unsigned long deepWork, unsigned long deepRest,
                                      unsigned long lightWork, unsigned long lightRest,
                                      unsigned long studyWork, unsigned long studyRest,
                                      float ineffectiveRatioVal) {
  workTime[STATE_DEEP_FOCUS]  = deepWork;
  workTime[STATE_LIGHT_WORK]  = lightWork;
  workTime[STATE_STUDY]       = studyWork;

  restTime[STATE_DEEP_FOCUS]  = deepRest;
  restTime[STATE_LIGHT_WORK]  = lightRest;
  restTime[STATE_STUDY]       = studyRest;
  restTime[STATE_DEEP_REST]   = deepRest;
  restTime[STATE_LIGHT_REST]  = lightRest;
  restTime[STATE_STUDY_REST]  = studyRest;

  ineffectiveRatio = ineffectiveRatioVal;

  Serial.println("[TIMER] runtime config applied");
}

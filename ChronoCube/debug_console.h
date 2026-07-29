#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

/* ============================================================
 * 维护约定：本文件与 debug_console.cpp 配套。
 * 权威使用/维护文档见 docs/serial_debug_console.md
 * 新增/修改命令时，必须同步更新该文档（命令表、回传格式、截屏行为），
 * 并在 debug_console.cpp 末尾的 s_cmds[] 分发表中登记。
 * ============================================================ */

#include "config.h"
#include "pose.h"
#include "keys.h"

/* ==================== Event Injection ====================
 * The debug console can inject simulated key and pose events
 * into the main event loop. Set these variables BEFORE calling
 * debugConsole_tick() — they are consumed by .ino's loop(). */
extern volatile KeyEvent debug_inject_key;
extern volatile PoseFace debug_inject_pose;
extern volatile bool debug_inject_pose_stable;

/* ==================== Watchdog (模块级活动监控) ==================== */

/* 在 loop() 的每个关键段之后调用。当设备卡死时，watchdog 命令可以
 * 告诉你 loop() 卡在哪个模块的哪段代码。 */
void debugWatchdog_poke(const char *module);

/* 在串口控制台中调用，打印所有模块的最后活动时间。 */
void debugWatchdog_dump(void);

/* 在 loop() 开始处调用，记录每次迭代的耗时。 */
void debugLoop_record(unsigned long now);

/* ==================== API ==================== */

/* Call from loop() to process serial commands.
 * When USE_LVGL is defined, the full debug console is active.
 * Otherwise, this is a no-op stub. */
void debugConsole_tick(void);

#endif /* DEBUG_CONSOLE_H */

/**
 * @file font_adapter.h
 * LVGL 中文点阵字体适配器（方向 A — SD 卡 cn_24.bin 自定义字体）
 *
 * 用法：
 *   1. cn_font_init("/sdcard/fonts/cn_24.bin");
 *   2. lv_obj_set_style_text_font(label, &cn_font, 0);
 *   3. lv_label_set_text(label, "你好世界");
 *
 * 硬件：ESP32-C6, 512KB RAM, 无 PSRAM, LVGL 8.3.11
 * 字库：24×24 点阵，Unicode 顺序，72 bytes/字
 * 缓存：8 槽 LRU，~576 B RAM
 *
 * 依赖：lvgl.h, <stdio.h> (fopen/fseek/fread)
 * 红线：无 STL / String / lambda / new / delay()
 */

#ifndef FONT_ADAPTER_H
#define FONT_ADAPTER_H

#include "lvgl.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Flash 内嵌字库开关（v5.5 移植） ==================== */
/* 1 = 从 Flash PROGMEM 数组读（无 SD 卡依赖，无 SPI2 总线冲突，首帧零卡顿）
 * 0 = 从 SD 卡 .bin 文件读（兼容 v5.3.12 旧路径） */
#define FONT_IN_FLASH   1

/* ==================== 字库参数 ==================== */

/* CN24: 24×24 点阵，72 bytes/字 */
#define CN24_GLYPH_W      24
#define CN24_GLYPH_H      24
#define CN24_BYTES_PER    72      /* 24×24÷8 */
#define CN24_BPP          1
#define CN24_CACHE_SLOTS  8       /* Flash 读快，8 槽够用，省 RAM */
#define CN24_UNICODE_BASE 0x0000

/* CN16: 16×16 点阵，CJK 统一汉字，32 bytes/字（旧字号，Flash 模式暂不内嵌） */
#define CN16_GLYPH_W      16
#define CN16_GLYPH_H      16
#define CN16_BYTES_PER    32      /* 16×16÷8 */
#define CN16_BPP          1
#define CN16_CACHE_SLOTS  16
#define CN16_UNICODE_BASE 0x4E00  /* CJK 统一汉字起始 */

/* CN48: 48×48 点阵，全 BMP 区段，288 bytes/字 */
#define CN48_GLYPH_W      48
#define CN48_GLYPH_H      48
#define CN48_BYTES_PER    288     /* 48×48÷8 */
#define CN48_BPP          1
#define CN48_CACHE_SLOTS  2       /* 288B×2=576B */
#define CN48_UNICODE_BASE 0x0000  /* 全 Unicode 区段 */

/* EN96: Cascadia Mono 数字，56×96 点阵，11 字形（0-9 + :），672 bytes/字 */
#define EN96_GLYPH_W      56
#define EN96_GLYPH_H      96
#define EN96_BYTES_PER    672     /* 56×96÷8 */
#define EN96_BPP          1
#define EN96_CACHE_SLOTS  2       /* 672B×2≈1.3KB */
#define EN96_UNICODE_BASE 0x20    /* ASCII 空格（兼容旧模式；Flash 模式下忽略，二分查找 codepoints 表） */

/* ==================== 类型定义 ==================== */

/**
 * 字体适配器内部状态（Flash 内嵌 / SD 卡 双模式）
 *   Flash 模式：file = NULL, codepoints != NULL, flash_data != NULL
 *   SD  模式  ：file != NULL, codepoints = NULL, flash_data = NULL
 * 缓存为动态分配（CN16=32B/slot，EN96=672B/slot，差异巨大）
 * 所有字段对调用方透明，通过 cn_font_init*() / cn_font_deinit() 管理
 */
typedef struct {
  FILE     *file;                  /* SD 模式：FILE* 句柄；Flash 模式：=NULL */
  size_t    fileSize;              /* SD 模式：文件字节数 */
  size_t    glyphCount;            /* 总字形数（SD 模式 = fileSize/bytesPer；Flash 模式 = codepoint_count） */

  /* ---- Flash 内嵌字库新增字段 ---- */
  const uint32_t *codepoints;      /* Flash 模式：码点表（升序，二分查找用），PROGMEM 只读 */
  size_t          codepoint_count; /* Flash 模式：码点表长度 = glyphCount */
  const uint8_t  *flash_data;      /* Flash 模式：点阵数据首地址，PROGMEM 只读，XIP 零拷贝 */
  uint16_t        flash_bytes_per; /* Flash 模式：每字字节数（= bytesPer，冗余方便） */

  uint8_t  *cacheData;             /* 动态分配：cacheSlots × bytesPer */
  uint32_t *cacheKey;
  uint8_t  *cacheAge;
  uint8_t   isReady;
  /* 运行时参数（cn_font_init_generic 设置） */
  uint8_t   glyphW;
  uint8_t   glyphH;
  uint16_t  bytesPer;              /* 必须 uint16_t，EN96=672 > 255 */
  uint8_t   bpp;
  uint8_t   cacheSlots;
  uint32_t  unicodeBase;           /* SD 模式用；Flash 模式忽略 */
  uint8_t   scale;                 /* 缩放因子：1=不缩放 */
} cn_font_state_t;

/* ==================== 全局字体实例 ==================== */

/** LVGL 字体对象，可传给 lv_obj_set_style_text_font() */
extern lv_font_t cn_font;

/** 48×48 中文字体 — 原生 48×48 点阵，从 cn_48.bin 加载，无缩放锯齿 */
extern lv_font_t cn_font_48;

/** 96px 英文数字字体 — 原生 56×96 点阵，从 en_96.bin 加载，用于计时/时钟大字 */
extern lv_font_t en_font_96;

/** 内部状态（供 init/deinit 使用，用户代码不应直接访问） */
extern cn_font_state_t cn_font_state;

/* ==================== API ==================== */

/**
 * 初始化中文字体适配器（SD 卡模式，FONT_IN_FLASH=0 时使用）
 * @param fontPath  SD 卡字库路径，如 "/cn_16.bin"
 * @return 0 成功，-1 文件打不开，-2 文件太小，-3 已初始化
 */
int cn_font_init(const char *fontPath);
int cn_font_48_init(const char *fontPath);
int en_font_96_init(const char *fontPath);

#if FONT_IN_FLASH
/**
 * Flash 内嵌模式初始化（v5.5）——不打开 SD 卡文件，从 PROGMEM 数组读
 *   v4 修正：CN24/CN48 各自绑定独立 codepoints 表（不再共用）
 *     CN24 → g_cn24_codepoints[69] (13ASCII+4标点+52汉字)
 *     CN48 → g_cn48_codepoints[29] (纯汉字)
 *     EN96 → g_en_codepoints[11]   (0-9+:)
 * @return 0 成功，-1 参数错误，-3 已初始化
 */
int cn_font_24_init_flash(void);   /* CN24: g_cn24_codepoints + g_cn24_data */
int cn_font_48_init_flash(void);   /* CN48: g_cn48_codepoints + g_cn48_data */
int en_font_96_init_flash(void);   /* EN96: g_en_codepoints  + g_en96_data */
#endif

/**
 * 释放资源（关闭文件，清理缓存）
 */
void cn_font_deinit(void);
void cn_font_48_deinit(void);
void en_font_96_deinit(void);

/**
 * 检查字体是否就绪
 * @return 1 就绪，0 未初始化
 */
uint8_t cn_font_is_ready(void);
uint8_t cn_font_48_is_ready(void);
uint8_t en_font_96_is_ready(void);

/** 预加载所有 UI 中文 glyph 到内存缓存
 *  ⚠️ FONT_IN_FLASH=1 时此函数为空实现（Flash XIP 已足够快，SD 冲突不存在）
 */
void cn_font_prewarm(void);

#ifdef __cplusplus
}
#endif

#endif /* FONT_ADAPTER_H */
#include "config.h"

#ifdef USE_LVGL

/**
 * @file font_adapter.cpp
 * LVGL 中文点阵字体适配器 — 16/24/32/48 四字号（48 原生，32 缩放）
 *
 * LVGL v9.5 适配：get_glyph_bitmap 回调签名改为 (lv_font_glyph_dsc_t*, lv_draw_buf_t*)，
 * bpp 字段已移除，改用 format (LV_FONT_GLYPH_FORMAT_A1)。
 * 新增 release_glyph / static_bitmap 字段支持。
 *
 * 字体层级：
 * cn_font_16 — 原生 16×16 (cn_16.bin), ~0.5KB 缓存
 * cn_font_24 — 原生 24×24 (cn_24.bin), ~4.6KB 缓存
 * cn_font_32 — 16×16 源 ×2 nearest-neighbor 放大, 共享 s_cn16 缓存
 * cn_font_48 — 原生 48×48 (cn_48.bin), ~14KB 缓存, 无锯齿
 *
 * 红线：无 STL / String / lambda / new / delay()
 */

#include "font_adapter.h"
#include "spi_bus_lock.h"

#if FONT_IN_FLASH
#include "font_flash_data.h"
#endif

#include <stdlib.h>  /* malloc, free */
#include <string.h>  /* memset, memcpy */

/* ==================== 内部状态（各字号独立） ==================== */

static cn_font_state_t s_cn16;
static cn_font_state_t s_cn24;
static cn_font_state_t s_cn48;
static cn_font_state_t s_en96;

/* ==================== LRU 缓存（通用） ==================== */

static void cache_init(cn_font_state_t *s) {
  memset(s->cacheKey, 0, s->cacheSlots * sizeof(uint32_t));
  memset(s->cacheAge, 0, s->cacheSlots * sizeof(uint8_t));
  memset(s->cacheData, 0, (size_t)s->cacheSlots * s->bytesPer);
}

static int cache_find(cn_font_state_t *s, uint32_t codepoint) {
  for (int i = 0; i < s->cacheSlots; i++) {
    if (s->cacheKey[i] == codepoint) {
      s->cacheAge[i] = 0;
      return i;
    }
  }
  return -1;
}

static int cache_evict(cn_font_state_t *s) {
  int oldest = 0;
  uint8_t maxAge = 0;
  for (int i = 0; i < s->cacheSlots; i++) {
    if (s->cacheKey[i] == 0) return i;
    if (s->cacheAge[i] > maxAge) {
      maxAge = s->cacheAge[i];
      oldest = i;
    }
  }
  for (int i = 0; i < s->cacheSlots; i++) {
    if (s->cacheAge[i] < 255) s->cacheAge[i]++;
  }
  return oldest;
}

#if FONT_IN_FLASH
/* ==================== Flash 模式：码点二分查找（核心） ====================
 *   因为自定义字库是 Unicode 子集（78 字 + 11 数字），不是连续区段，
 *   所以不能像 SD 模式那样直接 glyphIndex = cp - unicodeBase，
 *   必须用二分查找 O(log n) 定位。n=78 时最多 7 次比较，完全可忽略。
 *   codepoints 表必须严格升序（PROGMEM 只读，生成时保证）。
 *   返回值：>=0 = glyph index；-1 = 字库中不存在此字符（LVGL 渲染fallback/占位）。
 */
static int flash_glyph_index(const cn_font_state_t *s, uint32_t cp) {
  if (s->codepoints == NULL || s->codepoint_count == 0) return -1;

  int lo = 0;
  int hi = (int)s->codepoint_count - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;  /* 防止 int 溢出 */
    uint32_t mid_cp = (uint32_t)pgm_read_dword(&s->codepoints[mid]);
    if (mid_cp == cp)      return mid;
    else if (mid_cp < cp)  lo = mid + 1;
    else                   hi = mid - 1;
  }
  return -1;
}

/* 从 Flash PROGMEM 拷贝单 glyph 点阵 → cache slot（XIP 读，不占 RAM） */
static int flash_load_to_cache(cn_font_state_t *s, uint32_t cp, int slot) {
  int gi = flash_glyph_index(s, cp);
  if (gi < 0) return -1;
  if (s->flash_data == NULL) return -1;

  size_t off = (size_t)gi * (size_t)s->flash_bytes_per;
  /* pgm_read_block 从 Flash 拷贝，XIP 读 + memcpy_P → cache slot */
  memcpy_P(&s->cacheData[(size_t)slot * s->bytesPer],
           &s->flash_data[off],
           s->bytesPer);

  /* 检查空字形（与 SD 路径一致，防止占位 glyph 被误用） */
  uint8_t isEmpty = 1;
  uint8_t *slotData = &s->cacheData[(size_t)slot * s->bytesPer];
  for (int i = 0; i < s->bytesPer; i++) {
    if (slotData[i] != 0) { isEmpty = 0; break; }
  }
  if (isEmpty) return -1;

  s->cacheKey[slot] = cp;
  s->cacheAge[slot] = 0;
  return slot;
}
#endif /* FONT_IN_FLASH */

static int cache_load(cn_font_state_t *s, uint32_t codepoint) {
  if (!s->isReady) return -1;

#if FONT_IN_FLASH
  if (s->file == NULL && s->codepoints != NULL) {
    /* --- Flash 内嵌字库路径：二分查找 + XIP 读，不占 SPI2 --- */
    int slot = cache_evict(s);
    if (slot < 0) return -1;
    return flash_load_to_cache(s, codepoint, slot);
  }
#endif

  /* --- SD 卡路径（兼容旧代码）--- */
  if (!s->file) return -1;
  if (codepoint < s->unicodeBase) return -1;

  size_t glyphIndex = (size_t)(codepoint - s->unicodeBase);
  if (glyphIndex >= s->glyphCount) return -1;

  size_t offset = glyphIndex * s->bytesPer;
  if (offset + s->bytesPer > s->fileSize) return -1;

  int slot = cache_evict(s);
  if (slot < 0) return -1;

  /* P0: SPI2 总线互斥 — SD fread/fseek 与 LCD QSPI 共享 SPI2_HOST，必须串行化 */
  if (!spi2_lock()) {
    /* 锁超时：跳过本次加载，LVGL 会渲染 fallback 字体或方块 */
    return -1;
  }
  if (fseek(s->file, (long)offset, SEEK_SET) != 0) { spi2_unlock(); return -1; }
  size_t rd = fread(&s->cacheData[(size_t)slot * s->bytesPer], 1, s->bytesPer, s->file);
  spi2_unlock();

  if (rd != s->bytesPer) return -1;

  /* 检查空字形 */
  uint8_t isEmpty = 1;
  uint8_t *slotData = &s->cacheData[(size_t)slot * s->bytesPer];
  for (int i = 0; i < s->bytesPer; i++) {
    if (slotData[i] != 0) { isEmpty = 0; break; }
  }
  if (isEmpty) return -1;

  s->cacheKey[slot] = codepoint;
  s->cacheAge[slot] = 0;
  return slot;
}

/* ==================== LVGL 回调（v9 兼容） ==================== */

static bool cn_get_glyph_dsc_cb(const lv_font_t *font,
                                    lv_font_glyph_dsc_t *dsc,
                                    uint32_t unicode,
                                    uint32_t unicode_next) {
  (void)unicode_next;

  if (!font || !dsc) return false;

  cn_font_state_t *s = (cn_font_state_t *)font->dsc;
  if (!s || !s->isReady) return false;

#if FONT_IN_FLASH
  if (s->file == NULL && s->codepoints != NULL) {
    /* --- Flash 模式：必须先二分查找确认字库里有这个字 --- */
    int gi = flash_glyph_index(s, unicode);
    if (gi < 0) return false;
  } else
#endif
  {
    /* --- SD 卡模式：用 unicodeBase 连续区段判断 --- */
    if (unicode < s->unicodeBase) return false;
    size_t glyphIndex = (size_t)(unicode - s->unicodeBase);
    if (glyphIndex >= s->glyphCount) return false;
  }

  /* scale: user_data override 优先 → state.scale 默认=1 */
  uint8_t sc = s->scale;
  if (font->user_data != NULL) sc = *(const uint8_t *)font->user_data;

  dsc->adv_w = s->glyphW * sc;
  dsc->box_w = (int16_t)(s->glyphW * sc);
  dsc->box_h = (int16_t)(s->glyphH * sc);
  dsc->ofs_x = 0;
  dsc->ofs_y = 0;
  dsc->format = LV_FONT_GLYPH_FORMAT_A1;  /* LVGL 9.5 仅用此字段选渲染分支，实际渲染用 draw_buf->header.cf */
  dsc->is_placeholder = 0;
  dsc->gid.index = unicode;  /* 存储 unicode，供 get_glyph_bitmap 查找 */

  return true;
}

/* v9 签名：(lv_font_glyph_dsc_t *, lv_draw_buf_t *) — LVGL 9.5 要求返回 lv_draw_buf_t *
 *      内含已转为 A8 格式的字形位图。draw_buf 已由调用方分配（box_w × roundup(box_h,32) A8）。
 *      scale>1 时做 nearest-neighbor 整数放大，复用源缓存数据零额外 RAM。 */
static const void *cn_get_glyph_bitmap_cb(lv_font_glyph_dsc_t *g_dsc,
                                           lv_draw_buf_t *draw_buf) {
  if (!g_dsc || !g_dsc->resolved_font || !draw_buf) return NULL;

  cn_font_state_t *s = (cn_font_state_t *)g_dsc->resolved_font->dsc;
  if (!s || !s->isReady) return NULL;

  uint32_t unicode = g_dsc->gid.index;
  if (unicode < s->unicodeBase) return NULL;

  int slot = cache_find(s, unicode);
  if (slot < 0) {
    slot = cache_load(s, unicode);
  }
  if (slot < 0) return NULL;

  /* 获取 A1 原始位图 */
  const uint8_t *a1 = &s->cacheData[(size_t)slot * s->bytesPer];
  uint8_t *dest = draw_buf->data;
  if (!dest) return NULL;

  /* scale: user_data override 优先 → state.scale 默认=1 */
  uint8_t sc = s->scale;
  if (g_dsc->resolved_font->user_data != NULL)
    sc = *(const uint8_t *)g_dsc->resolved_font->user_data;

  int w = s->glyphW;
  int h = s->glyphH;
  int stride_a1 = (w + 7) / 8;                                /* A1: 源每行字节数 */

  int out_w = w * sc;
  int out_h = h * sc;
  int stride_a8 = (int)lv_draw_buf_width_to_stride((uint32_t)out_w, LV_COLOR_FORMAT_A8);

  if (sc == 1) {
    /* 1x: 原始尺寸 A1→A8 */
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int byteOff = y * stride_a1 + (x / 8);
        int bitOff  = 7 - (x % 8);
        dest[y * stride_a8 + x] = (a1[byteOff] & (1 << bitOff)) ? 0xFF : 0x00;
      }
    }
  } else {
    /* Nx: nearest-neighbor 整数放大 */
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int byteOff = y * stride_a1 + (x / 8);
        int bitOff  = 7 - (x % 8);
        uint8_t px = (a1[byteOff] & (1 << bitOff)) ? 0xFF : 0x00;
        /* 每个源像素 → sc×sc 输出像素块 */
        for (int dy = 0; dy < sc; dy++) {
          int destY = y * sc + dy;
          uint8_t *row = &dest[destY * stride_a8 + x * sc];
          for (int dx = 0; dx < sc; dx++) {
            row[dx] = px;
          }
        }
      }
    }
  }

  /* 填充 draw_buf 头部 */
  draw_buf->header.w    = (uint32_t)out_w;
  draw_buf->header.h    = (uint32_t)out_h;
  draw_buf->header.stride = (uint32_t)stride_a8;
  draw_buf->header.cf   = LV_COLOR_FORMAT_A8;

  return draw_buf;
}

/* ==================== LVGL 字体实例 ==================== */
/* 注：使用 C99 指定初始化器（GCC 扩展，Arduino ESP32 编译通过）。
 *     标准 C++ 无等价简洁写法，保留此风格以保证 lv_font_t 字段可读性。 */

static uint8_t SCALE_2X = 2;   /* user_data 整数缩放倍数 */

lv_font_t cn_font_16 = {
  .get_glyph_dsc    = cn_get_glyph_dsc_cb,
  .get_glyph_bitmap = cn_get_glyph_bitmap_cb,
  .release_glyph    = NULL,
  .line_height      = CN16_GLYPH_H,
  .base_line        = 0,
  .subpx            = LV_FONT_SUBPX_NONE,
  .kerning          = LV_FONT_KERNING_NONE,  /* 必须在 static_bitmap 前面 */
  .static_bitmap    = 0,  /* P0修复: LRU 缓存会被逐出，指针非永久有效，0=LVGL 不跨帧缓存 */
  .underline_position = -2,
  .underline_thickness = 1,
  .dsc              = &s_cn16,
  .fallback         = NULL,  /* 在 init 时设置 */
  .user_data        = NULL,
};

lv_font_t cn_font_24 = {
  .get_glyph_dsc    = cn_get_glyph_dsc_cb,
  .get_glyph_bitmap = cn_get_glyph_bitmap_cb,
  .release_glyph    = NULL,
  .line_height      = CN24_GLYPH_H,
  .base_line        = 0,
  .subpx            = LV_FONT_SUBPX_NONE,
  .kerning          = LV_FONT_KERNING_NONE,
  .static_bitmap    = 0,  /* P0修复: 同上 */
  .underline_position = -2,
  .underline_thickness = 1,
  .dsc              = &s_cn24,
  .fallback         = NULL,
  .user_data        = NULL,
};

/* ==================== 兼容旧 API：cn_font → cn_font_24 别名 ==================== */

lv_font_t cn_font = {
  .get_glyph_dsc    = cn_get_glyph_dsc_cb,
  .get_glyph_bitmap = cn_get_glyph_bitmap_cb,
  .release_glyph    = NULL,
  .line_height      = CN24_GLYPH_H,
  .base_line        = 0,
  .subpx            = LV_FONT_SUBPX_NONE,
  .kerning          = LV_FONT_KERNING_NONE,
  .static_bitmap    = 0,  /* P0修复: 同上 */
  .underline_position = -2,
  .underline_thickness = 1,
  .dsc              = &s_cn24,  /* 指向同一个 24×24 状态 */
  .fallback         = NULL,
  .user_data        = NULL,
};

/* ==================== 2x 缩放字体 — 共享源缓存，零额外 RAM ==================== */

lv_font_t cn_font_32 = {
  .get_glyph_dsc    = cn_get_glyph_dsc_cb,
  .get_glyph_bitmap = cn_get_glyph_bitmap_cb,
  .release_glyph    = NULL,
  .line_height      = CN16_GLYPH_H * 2,    /* 32 */
  .base_line        = 0,
  .subpx            = LV_FONT_SUBPX_NONE,
  .kerning          = LV_FONT_KERNING_NONE,
  .static_bitmap    = 0,
  .underline_position = -3,
  .underline_thickness = 1,
  .dsc              = &s_cn16,      /* 共享 16×16 源缓存 */
  .fallback         = NULL,
  .user_data        = &SCALE_2X,    /* 2x nearest-neighbor 放大 */
};

lv_font_t cn_font_48 = {
  .get_glyph_dsc    = cn_get_glyph_dsc_cb,
  .get_glyph_bitmap = cn_get_glyph_bitmap_cb,
  .release_glyph    = NULL,
  .line_height      = CN48_GLYPH_H,       /* 48 — 原生点阵，非缩放 */
  .base_line        = 0,
  .subpx            = LV_FONT_SUBPX_NONE,
  .kerning          = LV_FONT_KERNING_NONE,
  .static_bitmap    = 0,
  .underline_position = -4,
  .underline_thickness = 1,
  .dsc              = &s_cn48,      /* 独立 48×48 原生缓存 */
  .fallback         = NULL,         /* 在 init 时设置 */
  .user_data        = NULL,         /* scale=1，原生点阵，无缩放锯齿 */
};

/* ==================== 96px 英文数字字体 — 原生 56×96 点阵 ====================
 * ASCII 顺序：文件首字节 = 空格 (0x20)，连续排列到文件尾
 * 文件从 SD 卡 /sdcard/ChronoCube/fonts/en_96.bin 加载 */

lv_font_t en_font_96 = {
  .get_glyph_dsc    = cn_get_glyph_dsc_cb,
  .get_glyph_bitmap = cn_get_glyph_bitmap_cb,
  .release_glyph    = NULL,
  .line_height      = EN96_GLYPH_H,      /* 96 */
  .base_line        = 0,
  .subpx            = LV_FONT_SUBPX_NONE,
  .kerning          = LV_FONT_KERNING_NONE,
  .static_bitmap    = 0,
  .underline_position = -6,
  .underline_thickness = 2,
  .dsc              = &s_en96,           /* 独立 56×96 原生缓存 */
  .fallback         = NULL,              /* 在 init 时设置 */
  .user_data        = NULL,              /* scale=1，原生点阵 */
};

/* ==================== 通用初始化 ==================== */

static int cn_font_init_generic(cn_font_state_t *s, const char *fontPath,
                                 uint8_t w, uint8_t h, uint16_t bp, uint8_t cs,
                                 uint32_t unicodeBase) {
  if (!fontPath) return -1;
  if (s->isReady) return -3;

  s->glyphW = w;
  s->glyphH = h;
  s->bytesPer = bp;
  s->bpp = 1;
  s->cacheSlots = cs;
  s->unicodeBase = unicodeBase;
  s->scale = 1;  /* 默认 1x；cn_font_32/48 通过 user_data 覆盖为 2x */

  /* 动态分配缓存 */
  s->cacheData = (uint8_t *)malloc((size_t)cs * bp);
  s->cacheKey  = (uint32_t *)malloc((size_t)cs * sizeof(uint32_t));
  s->cacheAge  = (uint8_t *)malloc((size_t)cs * sizeof(uint8_t));

  if (!s->cacheData || !s->cacheKey || !s->cacheAge) {
    if (s->cacheData) { free(s->cacheData); s->cacheData = NULL; }
    if (s->cacheKey)  { free(s->cacheKey);  s->cacheKey  = NULL; }
    if (s->cacheAge)  { free(s->cacheAge);  s->cacheAge  = NULL; }
    return -1;
  }

  s->file = fopen(fontPath, "rb");
  if (!s->file) {
    free(s->cacheData); s->cacheData = NULL;
    free(s->cacheKey);  s->cacheKey  = NULL;
    free(s->cacheAge);  s->cacheAge  = NULL;
    return -1;
  }

  if (fseek(s->file, 0, SEEK_END) != 0) {
    fclose(s->file); s->file = NULL;
    free(s->cacheData); s->cacheData = NULL;
    free(s->cacheKey);  s->cacheKey  = NULL;
    free(s->cacheAge);  s->cacheAge  = NULL;
    return -1;
  }
  s->fileSize = (size_t)ftell(s->file);
  rewind(s->file);

  if (s->fileSize < bp) {
    fclose(s->file); s->file = NULL;
    s->fileSize = 0;
    free(s->cacheData); s->cacheData = NULL;
    free(s->cacheKey);  s->cacheKey  = NULL;
    free(s->cacheAge);  s->cacheAge  = NULL;
    return -2;
  }

  s->glyphCount = s->fileSize / bp;
  cache_init(s);
  s->isReady = 1;

  return 0;
}

static void cn_font_deinit_generic(cn_font_state_t *s) {
  if (s->file) {
    fclose(s->file);
    s->file = NULL;
  }
  if (s->cacheData) { free(s->cacheData); s->cacheData = NULL; }
  if (s->cacheKey)  { free(s->cacheKey);  s->cacheKey  = NULL; }
  if (s->cacheAge)  { free(s->cacheAge);  s->cacheAge  = NULL; }
  s->fileSize = 0;
  s->glyphCount = 0;
  s->codepoints = NULL;
  s->codepoint_count = 0;
  s->flash_data = NULL;
  s->flash_bytes_per = 0;
  s->isReady = 0;
}

#if FONT_IN_FLASH
/* ==================== Flash 模式：通用初始化（不打开SD，不占SPI2） ==================== */
static int cn_font_init_generic_flash(cn_font_state_t *s,
                                       uint8_t w, uint8_t h, uint16_t bp, uint8_t cs,
                                       const uint32_t *codepoints, size_t cp_count,
                                       const uint8_t *flash_data) {
  if (s->isReady) return -3;
  if (codepoints == NULL || cp_count == 0 || flash_data == NULL) return -1;

  s->glyphW = w;
  s->glyphH = h;
  s->bytesPer = bp;
  s->bpp = 1;
  s->cacheSlots = cs;
  s->unicodeBase = 0;      /* Flash 模式忽略 */
  s->scale = 1;

  s->file = NULL;
  s->fileSize = 0;
  s->codepoints      = codepoints;
  s->codepoint_count = cp_count;
  s->flash_data      = flash_data;
  s->flash_bytes_per = bp;
  s->glyphCount      = cp_count;  /* Flash 模式 glyphCount = codepoint 数 */

  /* 动态分配缓存（与 SD 路径共用同一套 LRU 机制） */
  s->cacheData = (uint8_t *)malloc((size_t)cs * bp);
  s->cacheKey  = (uint32_t *)malloc((size_t)cs * sizeof(uint32_t));
  s->cacheAge  = (uint8_t *)malloc((size_t)cs * sizeof(uint8_t));

  if (!s->cacheData || !s->cacheKey || !s->cacheAge) {
    if (s->cacheData) { free(s->cacheData); s->cacheData = NULL; }
    if (s->cacheKey)  { free(s->cacheKey);  s->cacheKey  = NULL; }
    if (s->cacheAge)  { free(s->cacheAge);  s->cacheAge  = NULL; }
    s->codepoints = NULL; s->flash_data = NULL;
    return -1;
  }
  cache_init(s);
  s->isReady = 1;
  return 0;
}

/* ==================== 三字号 Flash 初始化 API ====================
 *   v4 修正：CN24/CN48 各自绑定独立的 codepoints 表（不再共用）
 *     CN24 → g_cn24_codepoints[72] + g_cn24_data (含 ASCII+标点+汉字)
 *     CN48 → g_cn48_codepoints[29] + g_cn48_data (纯汉字)
 *     EN96 → g_en_codepoints[11]  + g_en96_data  (0-9+:)
 */
int cn_font_24_init_flash(void) {
  int ret = cn_font_init_generic_flash(&s_cn24,
                                         CN24_GLYPH_W, CN24_GLYPH_H, CN24_BYTES_PER, CN24_CACHE_SLOTS,
                                         g_cn24_codepoints, (size_t)G_CN24_COUNT,
                                         g_cn24_data);
  if (ret == 0) {
    cn_font_24.fallback = NULL;
    cn_font.fallback    = NULL;  /* 不回退 Montserrat，缺字显示占位符避免混大小 */
  }
  return ret;
}

int cn_font_48_init_flash(void) {
  int ret = cn_font_init_generic_flash(&s_cn48,
                                         CN48_GLYPH_W, CN48_GLYPH_H, CN48_BYTES_PER, CN48_CACHE_SLOTS,
                                         g_cn48_codepoints, (size_t)G_CN48_COUNT,
                                         g_cn48_data);
  if (ret == 0) {
    cn_font_48.fallback = &lv_font_montserrat_24;  /* CN48 大字如果缺字，降级到 24 蒙塞拉特 */
  }
  return ret;
}

int en_font_96_init_flash(void) {
  int ret = cn_font_init_generic_flash(&s_en96,
                                         EN96_GLYPH_W, EN96_GLYPH_H, EN96_BYTES_PER, EN96_CACHE_SLOTS,
                                         g_en_codepoints, (size_t)G_EN_COUNT,
                                         g_en96_data);
  if (ret == 0) {
    en_font_96.fallback = &lv_font_montserrat_48;  /* EN96 数字如果缺，降级到 48 蒙塞拉特 */
  }
  return ret;
}
#endif /* FONT_IN_FLASH */

/* ==================== 16×16 API ==================== */

int cn_font_16_init(const char *fontPath) {
  int ret = cn_font_init_generic(&s_cn16, fontPath,
                                  CN16_GLYPH_W, CN16_GLYPH_H,
                                  CN16_BYTES_PER, CN16_CACHE_SLOTS,
                                  CN16_UNICODE_BASE);
  if (ret == 0) {
    cn_font_16.fallback = &lv_font_montserrat_12;
  }
  return ret;
}

void cn_font_16_deinit(void) {
  cn_font_deinit_generic(&s_cn16);
}

uint8_t cn_font_16_is_ready(void) {
  return s_cn16.isReady;
}

/* ==================== 24×24 API ==================== */

int cn_font_24_init(const char *fontPath) {
  int ret = cn_font_init_generic(&s_cn24, fontPath,
                                  CN24_GLYPH_W, CN24_GLYPH_H,
                                  CN24_BYTES_PER, CN24_CACHE_SLOTS,
                                  CN24_UNICODE_BASE);
  if (ret == 0) {
    cn_font_24.fallback = NULL;
    cn_font.fallback = NULL;  /* 不回退，ASCII/缺失字符渲染占位符 */
  }
  return ret;
}

void cn_font_24_deinit(void) {
  cn_font_deinit_generic(&s_cn24);
}

uint8_t cn_font_24_is_ready(void) {
  return s_cn24.isReady;
}

/* ==================== 48×48 API ==================== */

int cn_font_48_init(const char *fontPath) {
  int ret = cn_font_init_generic(&s_cn48, fontPath,
                                  CN48_GLYPH_W, CN48_GLYPH_H,
                                  CN48_BYTES_PER, CN48_CACHE_SLOTS,
                                  CN48_UNICODE_BASE);
  if (ret == 0) {
    cn_font_48.fallback = &lv_font_montserrat_24;
  }
  return ret;
}

void cn_font_48_deinit(void) {
  cn_font_deinit_generic(&s_cn48);
}

uint8_t cn_font_48_is_ready(void) {
  return s_cn48.isReady;
}

/* ==================== 96px 英文 API ==================== */

int en_font_96_init(const char *fontPath) {
  int ret = cn_font_init_generic(&s_en96, fontPath,
                                  EN96_GLYPH_W, EN96_GLYPH_H,
                                  EN96_BYTES_PER, EN96_CACHE_SLOTS,
                                  EN96_UNICODE_BASE);
  if (ret == 0) {
    en_font_96.fallback = &lv_font_montserrat_48;
  }
  return ret;
}

void en_font_96_deinit(void) {
  cn_font_deinit_generic(&s_en96);
}

uint8_t en_font_96_is_ready(void) {
  return s_en96.isReady;
}

/* ==================== 兼容旧 API ==================== */

int cn_font_init(const char *fontPath) {
  return cn_font_24_init(fontPath);
}

void cn_font_deinit(void) {
  cn_font_24_deinit();
}

uint8_t cn_font_is_ready(void) {
  return cn_font_24_is_ready();
}

/* ==================== 字形预加载（P0 修复：避免首次渲染 SD+QSPI 冲突）==================== */

static uint32_t utf8_decode(const char *s, int *outLen) {
  unsigned char c = (unsigned char)*s;
  if (c < 0x80)         { *outLen = 1; return c; }
  if ((c & 0xE0) == 0xC0) { *outLen = 2; return ((c & 0x1F) << 6)  | (s[1] & 0x3F); }
  if ((c & 0xF0) == 0xE0) { *outLen = 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
  *outLen = 1;
  return c;
}

#define PREWARM_MAX_UNIQUE 128

void cn_font_prewarm(void) {
#if FONT_IN_FLASH
  /* Flash 内嵌字库：
   *   XIP 读 ~10ns，首次渲染单 glyph < 0.2ms，且不占 SPI2，SD/LCD 冲突不存在。
   *   直接跳过 prewarm，省启动时间 ~60ms。
   *   — 实测 ChronoCube 78字 × 3字号 prewarm 总耗时：62ms → 0ms ✅
   */
  return;
#else
  /* 所有 UI 屏幕中出现的中文 strings（含锁屏日期部件） */
  static const char *strings[] = {
    "深度专注", "轻量事务", "学习成长",
    "深度休息", "轻量休息", "学习休息",
    "今日", "已暂停", "感觉如何？",
    "顺畅", "平淡", "卡壳", "耗竭",
    "高效专注", "低效专注", "已锁定",
    "休息暂停", "电量不足，请充电",
    "后自动选择",  /* S8 倒计时提示 "10s后自动选择「平淡」" */
    "月", "日", "一", "二", "三", "四", "五", "六",
    "天", "小", "时", "分", "秒", "年",
    "周", "第",     /* 锁屏日期 "周五 07.17 第29周" — 原生 UI drawChinese 路径也读 SD 字库 */
    NULL
  };

  uint32_t unique[PREWARM_MAX_UNIQUE];
  int uniqueCount = 0;

  for (int i = 0; strings[i] != NULL; i++) {
    const char *s = strings[i];
    while (*s) {
      int len;
      uint32_t cp = utf8_decode(s, &len);
      if (len >= 2 && cp >= 0x4E00 && cp <= 0x9FFF) { /* CJK Unified Ideographs */
        bool found = false;
        for (int j = 0; j < uniqueCount; j++) {
          if (unique[j] == cp) { found = true; break; }
        }
        if (!found && uniqueCount < PREWARM_MAX_UNIQUE) {
          unique[uniqueCount++] = cp;
        }
      }
      s += len;
    }
  }

  int loaded24 = 0, loaded16 = 0, loaded48 = 0;
  for (int i = 0; i < uniqueCount; i++) {
    if (s_cn24.isReady) {
      int slot = cache_find(&s_cn24, unique[i]);
      if (slot < 0) {
        slot = cache_load(&s_cn24, unique[i]);
        if (slot >= 0) loaded24++;
      }
    }
    if (s_cn16.isReady) {
      int slot = cache_find(&s_cn16, unique[i]);
      if (slot < 0) {
        slot = cache_load(&s_cn16, unique[i]);
        if (slot >= 0) loaded16++;
      }
    }
    if (s_cn48.isReady) {
      int slot = cache_find(&s_cn48, unique[i]);
      if (slot < 0) {
        slot = cache_load(&s_cn48, unique[i]);
        if (slot >= 0) loaded48++;
      }
    }
  }

  if (loaded24 > 0 || loaded16 > 0 || loaded48 > 0) {
    Serial.printf("[FONT] prewarm: %d unique CJK chars → cn48:%d cn24:%d cn16:%d (cache %d/%d/%d slots)\n",
                  uniqueCount, loaded48, loaded24, loaded16,
                  (s_cn48.isReady ? s_cn48.cacheSlots : 0),
                  (s_cn24.isReady ? s_cn24.cacheSlots : 0),
                  (s_cn16.isReady ? s_cn16.cacheSlots : 0));
  }

  /* 预热 96px 英文数字：0-9 + : = 11 个 glyph，用于计时/时钟大字 */
  {
    static const uint32_t en96Chars[] = {
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':'
    };
    int loaded96 = 0;
    if (s_en96.isReady) {
      for (int i = 0; i < (int)(sizeof(en96Chars) / sizeof(en96Chars[0])); i++) {
        int slot = cache_find(&s_en96, en96Chars[i]);
        if (slot < 0) {
          slot = cache_load(&s_en96, en96Chars[i]);
          if (slot >= 0) loaded96++;
        }
      }
    }
    if (loaded96 > 0) {
      Serial.printf("[FONT] prewarm: en_96 loaded %d/11 digit glyphs (cache %d slots)\n",
                    loaded96, s_en96.cacheSlots);
    }
  }
#endif /* !FONT_IN_FLASH */
}

#endif /* USE_LVGL */

#ifndef FONT_LOADER_H
#define FONT_LOADER_H

#include <Arduino.h>
#include <stdio.h>

// ========== 字库格式说明 ==========
// 方案 B：中英双字库，固定宽度
//   中文字库 (cn_24.bin)：
//     - 按 Unicode 码点顺序排列
//     - 每字：24×24 点阵 = 72 字节（24×24÷8=72）
//     - 固定宽度 24px，高度 24px
//   英文字库 (en_24.bin)：
//     - 按 ASCII 码点顺序排列（32-127，共 96 个字符）
//     - 每字：12×24 点阵 = 48 字节（24行 × 2字节/行，每行16位低12位有效）
//     - 固定宽度 12px，高度 24px
//     - 索引 = (ascii_byte - 32) * 48

// ===== 中文字库参数 =====
#define CN_FONT_W      24    // 中文宽度
#define CN_FONT_H      24    // 中文高度
#define CN_BYTES_PER_GLYPH  72    // 24×24÷8 = 72

// ===== 英文字库参数 =====
#define EN_FONT_W      12    // 英文宽度
#define EN_FONT_H      24    // 英文高度
#define EN_BYTES_PER_GLYPH  48    // 24行 × 2字节/行 = 48（每行16位，低12位有效）
#define EN_FONT_GLYPH_COUNT  96    // ASCII 32-127

// ===== 缓存 =====
#define CN_CACHE_SIZE  128   // 中文 LRU 缓存槽数
#define EN_CACHE_SIZE  64    // 英文 LRU 缓存槽数

class FontLoader {
public:
  bool beginCN(const char *fontPath = "/sdcard/ChronoCube/fonts/cn_24.bin");
  bool beginEN(const char *fontPath = "/sdcard/ChronoCube/fonts/en_24.bin");
  bool isCNReady() const;
  bool isENReady() const;
  void end();

  // 中文：UTF-8 → 24×24 点阵（72 字节）
  const uint8_t* getCNGlyph(const uint8_t *utf8, int len);
  // 英文：ASCII 字节 → 12×24 点阵（48 字节）
  const uint8_t* getENGlyph(uint8_t ascii);

  // 检查指针是否在中文或英文缓存中
  bool isInCNCache(const uint8_t *ptr) const;
  bool isInENCache(const uint8_t *ptr) const;

  // 缓存命中率统计
  void getCacheStats(uint32_t &cnHits, uint32_t &cnMisses,
                     uint32_t &enHits, uint32_t &enMisses) const;

private:
  bool cnReady, enReady;
  FILE *cnFile, *enFile;
  size_t cnFileSize, enFileSize;
  size_t cnGlyphCount;

  static uint32_t utf8ToCodepoint(const uint8_t *utf8, int len);
};

extern FontLoader fontLoader;

#endif

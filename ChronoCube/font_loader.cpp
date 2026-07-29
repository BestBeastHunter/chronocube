#include "font_loader.h"
#include "spi_bus_lock.h"
#include <stdio.h>

FontLoader fontLoader;

// === 缓存命中率统计 ===
static uint32_t sCnHits   = 0;
static uint32_t sCnMisses = 0;
static uint32_t sEnHits   = 0;
static uint32_t sEnMisses = 0;

// ========== UTF-8 码点提取 ==========
uint32_t FontLoader::utf8ToCodepoint(const uint8_t *utf8, int len) {
  if (len == 1) return utf8[0];
  if (len == 2) {
    if ((utf8[0] & 0xE0) != 0xC0) return 0;
    if ((utf8[1] & 0xC0) != 0x80) return 0;
    return ((uint32_t)(utf8[0] & 0x1F) << 6) | ((uint32_t)(utf8[1] & 0x3F));
  }
  if (len == 3) {
    if ((utf8[0] & 0xF0) != 0xE0) return 0;
    if ((utf8[1] & 0xC0) != 0x80) return 0;
    if ((utf8[2] & 0xC0) != 0x80) return 0;
    return ((uint32_t)(utf8[0] & 0x0F) << 12) |
           ((uint32_t)(utf8[1] & 0x3F) << 6) |
           ((uint32_t)(utf8[2] & 0x3F));
  }
  return 0;
}

// ========== 中文 LRU 缓存 ==========
static uint8_t  cnCacheData[CN_CACHE_SIZE][CN_BYTES_PER_GLYPH];
static uint32_t cnCacheKeys[CN_CACHE_SIZE];
static uint8_t  cnCacheAge[CN_CACHE_SIZE];

static void cnCacheInit() {
  for (int i = 0; i < CN_CACHE_SIZE; i++) {
    cnCacheKeys[i] = 0;
    cnCacheAge[i] = 0;
  }
}

static int cnCacheFind(uint32_t codepoint) {
  for (int i = 0; i < CN_CACHE_SIZE; i++) {
    if (cnCacheKeys[i] == codepoint) {
      cnCacheAge[i] = 0;
      return i;
    }
  }
  return -1;
}

static int cnCacheEvict() {
  int oldest = 0;
  uint8_t maxAge = 0;
  for (int i = 0; i < CN_CACHE_SIZE; i++) {
    if (cnCacheKeys[i] == 0) return i;
    if (cnCacheAge[i] > maxAge) {
      maxAge = cnCacheAge[i];
      oldest = i;
    }
  }
  for (int i = 0; i < CN_CACHE_SIZE; i++) {
    if (cnCacheAge[i] < 255) cnCacheAge[i]++;
  }
  return oldest;
}

// ========== 英文 LRU 缓存 ==========
static uint8_t  enCacheData[EN_CACHE_SIZE][EN_BYTES_PER_GLYPH];
static uint8_t  enCacheKeys[EN_CACHE_SIZE];  // 存 ASCII 码 (32-127)
static uint8_t  enCacheAge[EN_CACHE_SIZE];

static void enCacheInit() {
  for (int i = 0; i < EN_CACHE_SIZE; i++) {
    enCacheKeys[i] = 0;
    enCacheAge[i] = 0;
  }
}

static int enCacheFind(uint8_t ascii) {
  for (int i = 0; i < EN_CACHE_SIZE; i++) {
    if (enCacheKeys[i] == ascii) {
      enCacheAge[i] = 0;
      return i;
    }
  }
  return -1;
}

static int enCacheEvict() {
  int oldest = 0;
  uint8_t maxAge = 0;
  for (int i = 0; i < EN_CACHE_SIZE; i++) {
    if (enCacheKeys[i] == 0) return i;
    if (enCacheAge[i] > maxAge) {
      maxAge = enCacheAge[i];
      oldest = i;
    }
  }
  for (int i = 0; i < EN_CACHE_SIZE; i++) {
    if (enCacheAge[i] < 255) enCacheAge[i]++;
  }
  return oldest;
}

// ========== 中文字库初始化 ==========
bool FontLoader::beginCN(const char *fontPath) {
  cnReady = false;
  cnFile = nullptr;
  cnFileSize = 0;
  cnGlyphCount = 0;

  cnFile = fopen(fontPath, "rb");
  if (!cnFile) {
    Serial.printf("[FONT] cannot open CN: %s\n", fontPath);
    return false;
  }

  fseek(cnFile, 0, SEEK_END);
  cnFileSize = ftell(cnFile);
  fseek(cnFile, 0, SEEK_SET);

  if (cnFileSize < CN_BYTES_PER_GLYPH) {
    Serial.println("[FONT] CN file too small");
    fclose(cnFile);
    cnFile = nullptr;
    return false;
  }

  cnGlyphCount = cnFileSize / CN_BYTES_PER_GLYPH;
  Serial.printf("[FONT] CN opened %s (%u glyphs, %u bytes, 24x24 fixed)\n",
    fontPath, cnGlyphCount, cnFileSize);

  cnCacheInit();

  cnReady = true;
  Serial.printf("[FONT] CN ready: cache=%d glyphs, %d bytes RAM\n",
    CN_CACHE_SIZE, CN_CACHE_SIZE * CN_BYTES_PER_GLYPH);
  return true;
}

// ========== 英文字库初始化 ==========
bool FontLoader::beginEN(const char *fontPath) {
  enReady = false;
  enFile = nullptr;
  enFileSize = 0;

  enFile = fopen(fontPath, "rb");
  if (!enFile) {
    Serial.printf("[FONT] cannot open EN: %s\n", fontPath);
    return false;
  }

  fseek(enFile, 0, SEEK_END);
  enFileSize = ftell(enFile);
  fseek(enFile, 0, SEEK_SET);

  size_t expected = (size_t)EN_FONT_GLYPH_COUNT * EN_BYTES_PER_GLYPH;
  if (enFileSize < expected) {
    Serial.printf("[FONT] EN file too small: %u < %u expected\n", enFileSize, expected);
    fclose(enFile);
    enFile = nullptr;
    return false;
  }

  Serial.printf("[FONT] EN opened %s (%u glyphs, %u bytes, 12x24 fixed)\n",
    fontPath, EN_FONT_GLYPH_COUNT, enFileSize);

  enCacheInit();

  enReady = true;
  Serial.printf("[FONT] EN ready: cache=%d glyphs, %d bytes RAM\n",
    EN_CACHE_SIZE, EN_CACHE_SIZE * EN_BYTES_PER_GLYPH);
  return true;
}

bool FontLoader::isCNReady() const {
  return cnReady && cnFile != nullptr;
}

bool FontLoader::isENReady() const {
  return enReady && enFile != nullptr;
}

void FontLoader::end() {
  if (cnFile) { fclose(cnFile); cnFile = nullptr; }
  if (enFile) { fclose(enFile); enFile = nullptr; }
  cnReady = enReady = false;
  cnFileSize = enFileSize = 0;
  cnGlyphCount = 0;
  cnCacheInit();
  enCacheInit();
  sCnHits = sCnMisses = sEnHits = sEnMisses = 0;
}

// ========== 缓存统计 ==========
void FontLoader::getCacheStats(uint32_t &cnHits, uint32_t &cnMisses,
                               uint32_t &enHits, uint32_t &enMisses) const {
  cnHits = sCnHits; cnMisses = sCnMisses;
  enHits = sEnHits; enMisses = sEnMisses;
}

// ========== 中文字形读取（24×24，72字节）==========
const uint8_t* FontLoader::getCNGlyph(const uint8_t *utf8, int len) {
  if (!cnReady || !cnFile || !utf8 || len <= 0) return nullptr;

  uint32_t codepoint = utf8ToCodepoint(utf8, len);
  if (codepoint >= cnGlyphCount) return nullptr;

  // 检查缓存
  int cacheIdx = cnCacheFind(codepoint);
  if (cacheIdx >= 0) {
    sCnHits++;
    return cnCacheData[cacheIdx];
  }
  sCnMisses++;

  // 缓存未命中，从 SD 卡读取
  size_t offset = (size_t)codepoint * CN_BYTES_PER_GLYPH;
  if (offset + CN_BYTES_PER_GLYPH > cnFileSize) return nullptr;

  int slot = cnCacheEvict();

  if (!spi2_lock()) return nullptr;
  if (fseek(cnFile, offset, SEEK_SET) != 0) { spi2_unlock(); return nullptr; }
  size_t rd = fread(cnCacheData[slot], 1, CN_BYTES_PER_GLYPH, cnFile);
  spi2_unlock();
  if (rd != CN_BYTES_PER_GLYPH) return nullptr;

  // 检查是否为空字形（点阵全零）
  bool isEmpty = true;
  for (int i = 0; i < CN_BYTES_PER_GLYPH; i++) {
    if (cnCacheData[slot][i] != 0) {
      isEmpty = false;
      break;
    }
  }
  if (isEmpty) return nullptr;

  cnCacheKeys[slot] = codepoint;
  cnCacheAge[slot] = 0;

  return cnCacheData[slot];
}

// ========== 英文字形读取（12×24，48字节）==========
const uint8_t* FontLoader::getENGlyph(uint8_t ascii) {
  if (!enReady || !enFile) return nullptr;

  if (ascii < 32 || ascii > 127) return nullptr;

  // 检查缓存
  int cacheIdx = enCacheFind(ascii);
  if (cacheIdx >= 0) {
    sEnHits++;
    return enCacheData[cacheIdx];
  }
  sEnMisses++;

  // 缓存未命中，从 SD 卡读取
  // 索引 = (ascii - 32) * 48，文件只存 32-127 共 96 字符
  uint8_t idx = ascii - 32;
  size_t offset = (size_t)idx * EN_BYTES_PER_GLYPH;
  if (offset + EN_BYTES_PER_GLYPH > enFileSize) return nullptr;

  int slot = enCacheEvict();

  if (!spi2_lock()) return nullptr;
  if (fseek(enFile, offset, SEEK_SET) != 0) { spi2_unlock(); return nullptr; }
  size_t rd = fread(enCacheData[slot], 1, EN_BYTES_PER_GLYPH, enFile);
  spi2_unlock();
  if (rd != EN_BYTES_PER_GLYPH) return nullptr;

  // 检查是否为空字形
  bool isEmpty = true;
  for (int i = 0; i < EN_BYTES_PER_GLYPH; i++) {
    if (enCacheData[slot][i] != 0) {
      isEmpty = false;
      break;
    }
  }
  if (isEmpty) return nullptr;

  enCacheKeys[slot] = ascii;
  enCacheAge[slot] = 0;

  return enCacheData[slot];
}

// ========== 缓存指针检查 ==========
bool FontLoader::isInCNCache(const uint8_t *ptr) const {
  if (!ptr) return false;
  for (int i = 0; i < CN_CACHE_SIZE; i++) {
    if (ptr >= &cnCacheData[i][0] && ptr < &cnCacheData[i][CN_BYTES_PER_GLYPH]) {
      return true;
    }
  }
  return false;
}

bool FontLoader::isInENCache(const uint8_t *ptr) const {
  if (!ptr) return false;
  for (int i = 0; i < EN_CACHE_SIZE; i++) {
    if (ptr >= &enCacheData[i][0] && ptr < &enCacheData[i][EN_BYTES_PER_GLYPH]) {
      return true;
    }
  }
  return false;
}

#include "wav_io.h"
#include <ctype.h>
#include <string.h>
#include <SPI.h>

// ---------------------------------------------------------------------------
bool tcSdBegin() {
  if (SD.begin(BUILTIN_SDCARD)) {
    Serial.println(F("[SD] 使用 Teensy 4.1 內建 SD (SDIO)"));
    return true;
  }
  SPI.setMOSI(TC_SDCARD_MOSI_PIN);
  SPI.setSCK(TC_SDCARD_SCK_PIN);
  if (SD.begin(TC_SDCARD_CS_PIN)) {
    Serial.println(F("[SD] 使用 Audio Shield SD (SPI)"));
    return true;
  }
  Serial.println(F("[SD] 初始化失敗，請確認卡片已插入並格式化為 FAT32/exFAT"));
  return false;
}

// ---------------------------------------------------------------------------
void tcSdList() {
  File dir = SD.open("/");
  if (!dir) {
    Serial.println(F("[SD] 打不開根目錄 —— 卡片沒掛起來。"));
    Serial.println(F("     檢查：卡有沒有插到底、格式是不是 FAT32/exFAT。"));
    return;
  }

  Serial.println(F("---------------- SD 根目錄 ----------------"));
  int n = 0;
  uint32_t total = 0;
  while (true) {
    File e = dir.openNextFile();
    if (!e) break;
    const char *nm = e.name();
    if (nm && nm[0] != '.') {                       // skip macOS hidden files
      if (e.isDirectory()) {
        Serial.printf("  %-24s      <DIR>\n", nm);
      } else {
        Serial.printf("  %-24s %9lu bytes\n", nm, (unsigned long)e.size());
        total += e.size();
      }
      n++;
    }
    e.close();
  }
  dir.close();

  if (n == 0) Serial.println(F("  (空的)"));
  Serial.printf("  共 %d 個項目，%lu bytes\n", n, (unsigned long)total);
  Serial.println(F("------------------------------------------"));
  Serial.println(F("提示：分析請用「a 檔名.WAV」，直接按 a 是找 REC.WAV（要先按 r 錄音）"));
}

// ---------------------------------------------------------------------------
static bool endsWithWav(const char *n) {
  int L = (int)strlen(n);
  if (L < 5) return false;
  const char *e = n + L - 4;
  return (e[0] == '.') &&
         (e[1] == 'W' || e[1] == 'w') &&
         (e[2] == 'A' || e[2] == 'a') &&
         (e[3] == 'V' || e[3] == 'v');
}

static bool sameNameCI(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'a' && ca <= 'z') ca -= 32;
    if (cb >= 'a' && cb <= 'z') cb -= 32;
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

int tcSdCollectWavs(const char *dir, char *outNames, int maxCount, const char *skipName) {
  File d = SD.open(dir && dir[0] ? dir : "/");
  if (!d) { Serial.printf("[SD] 打不開目錄 %s\n", dir); return 0; }

  int n = 0;
  while (n < maxCount) {
    File e = d.openNextFile();
    if (!e) break;
    const char *nm = e.name();
    bool isDir = e.isDirectory();
    if (nm && !isDir && nm[0] != '.' && endsWithWav(nm) &&
        !(skipName && sameNameCI(nm, skipName))) {
      // Skip an over-long filename entirely rather than truncate it -- a
      // truncated name always fails to open later, and turns into a baffling
      // "the file is plainly there but cannot be found".
      if (strlen(nm) >= TC_MAX_NAME_LEN) {
        Serial.printf("[SD] 檔名過長，跳過：%s（上限 %d 字元）\n", nm, TC_MAX_NAME_LEN - 1);
      } else {
        char *slot = outNames + (size_t)n * TC_MAX_NAME_LEN;
        memcpy(slot, nm, strlen(nm) + 1);
        n++;
      }
    }
    e.close();
  }
  d.close();

  // Sort by filename so the load order is identical on every run (so training results reproduce)
  for (int i = 1; i < n; i++) {
    char key[TC_MAX_NAME_LEN];
    snprintf(key, TC_MAX_NAME_LEN, "%s", outNames + (size_t)i * TC_MAX_NAME_LEN);
    int j = i - 1;
    while (j >= 0 && strcmp(outNames + (size_t)j * TC_MAX_NAME_LEN, key) > 0) {
      memcpy(outNames + (size_t)(j + 1) * TC_MAX_NAME_LEN,
             outNames + (size_t)j * TC_MAX_NAME_LEN, TC_MAX_NAME_LEN);
      j--;
    }
    memcpy(outNames + (size_t)(j + 1) * TC_MAX_NAME_LEN, key, TC_MAX_NAME_LEN);
  }
  return n;
}

bool tcSdCopy(const char *src, const char *dst) {
  File in = SD.open(src, FILE_READ);
  if (!in) { Serial.printf("[SD] 讀不到 %s\n", src); return false; }
  if (SD.exists(dst)) SD.remove(dst);
  File out = SD.open(dst, FILE_WRITE);
  if (!out) { in.close(); Serial.printf("[SD] 建不了 %s\n", dst); return false; }

  static uint8_t buf[512];
  uint32_t total = 0;
  while (true) {
    int n = in.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.write(buf, n);
    total += (uint32_t)n;
  }
  in.close();
  out.close();
  Serial.printf("[SD] %s -> %s (%lu KB)\n", src, dst, (unsigned long)(total / 1024));
  return total > 44;
}

// ----------------------------------------------------------------- helpers --
static uint32_t rd32(File &f) {
  uint8_t b[4];
  f.read(b, 4);
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint16_t rd16(File &f) {
  uint8_t b[2];
  f.read(b, 2);
  return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

// ==================================================================== read ==
bool WavReader::open(const char *path) {
  close();
  _f = SD.open(path, FILE_READ);
  if (!_f) {
    Serial.printf("[WAV] 找不到檔案 %s\n", path);
    return false;
  }

  char tag[5] = {0};
  _f.read((uint8_t *)tag, 4);
  if (strncmp(tag, "RIFF", 4) != 0) { Serial.println(F("[WAV] 不是 RIFF")); _f.close(); return false; }
  rd32(_f);                                   // RIFF size
  _f.read((uint8_t *)tag, 4);
  if (strncmp(tag, "WAVE", 4) != 0) { Serial.println(F("[WAV] 不是 WAVE")); _f.close(); return false; }

  bool haveFmt = false;
  while (_f.available() >= 8) {
    _f.read((uint8_t *)tag, 4);
    uint32_t sz = rd32(_f);
    uint32_t next = _f.position() + sz + (sz & 1);

    if (strncmp(tag, "fmt ", 4) == 0) {
      uint16_t fmt = rd16(_f);
      _channels    = rd16(_f);
      _sampleRate  = rd32(_f);
      rd32(_f);                                // byte rate
      rd16(_f);                                // block align
      _bits        = rd16(_f);
      if ((fmt != 1 && fmt != 0xFFFE) || _bits != 16) {
        Serial.printf("[WAV] 只支援 16-bit PCM (fmt=%u bits=%u)\n", fmt, _bits);
        _f.close();
        return false;
      }
      haveFmt = true;
    } else if (strncmp(tag, "data", 4) == 0) {
      if (!haveFmt) { _f.close(); return false; }
      _dataOffset = _f.position();
      _frames     = sz / (2u * _channels);
      _open       = true;
      Serial.printf("[WAV] %s  %u Hz  %u ch  %lu 取樣 (%.2f 秒)\n",
                    path, (unsigned)_sampleRate, (unsigned)_channels,
                    (unsigned long)_frames, _frames / (float)_sampleRate);
      return true;
    }
    _f.seek(next);
  }
  Serial.println(F("[WAV] 找不到 data chunk"));
  _f.close();
  return false;
}

void WavReader::close() {
  if (_f) _f.close();
  _open = false;
}

uint32_t WavReader::readMono(uint32_t frameIndex, float *dst, uint32_t n) {
  if (!_open) return 0;
  if (frameIndex >= _frames) { for (uint32_t i = 0; i < n; i++) dst[i] = 0.0f; return 0; }

  uint32_t avail = _frames - frameIndex;
  uint32_t want  = (n < avail) ? n : avail;

  _f.seek(_dataOffset + (uint32_t)frameIndex * 2u * _channels);

  static int16_t buf[256];
  const uint32_t chunkFrames = (_channels == 1) ? 256 : 128;

  uint32_t done = 0;
  while (done < want) {
    uint32_t k = want - done;
    if (k > chunkFrames) k = chunkFrames;
    int got = _f.read((uint8_t *)buf, k * 2 * _channels);
    if (got <= 0) break;
    uint32_t gotFrames = (uint32_t)got / (2u * _channels);
    for (uint32_t i = 0; i < gotFrames; i++) {
      int32_t s;
      if (_channels == 1) s = buf[i];
      else                s = ((int32_t)buf[i * 2] + (int32_t)buf[i * 2 + 1]) / 2;   // mix down to mono
      dst[done + i] = (float)s * (1.0f / 32768.0f);
    }
    done += gotFrames;
    if (gotFrames < k) break;
  }
  for (uint32_t i = done; i < n; i++) dst[i] = 0.0f;      // zero-pad
  return done;
}

// =================================================================== write ==
static void wr32(File &f, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  f.write(b, 4);
}
static void wr16(File &f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  f.write(b, 2);
}

bool WavWriter::open(const char *path, uint32_t sampleRate, uint16_t channels,
                     uint32_t expectedSamples) {
  if (SD.exists(path)) SD.remove(path);
  _f = SD.open(path, FILE_WRITE);
  if (!_f) { Serial.printf("[WAV] 無法建立 %s\n", path); return false; }

  _sampleRate = sampleRate;
  _channels   = channels;
  _dataBytes  = 0;

  const uint32_t preBytes = expectedSamples * 2u;   // the estimated data length written up front
  _f.write((const uint8_t *)"RIFF", 4);  wr32(_f, 36 + preBytes);
  _f.write((const uint8_t *)"WAVE", 4);
  _f.write((const uint8_t *)"fmt ", 4);  wr32(_f, 16);
  wr16(_f, 1);                       // PCM
  wr16(_f, channels);
  wr32(_f, sampleRate);
  wr32(_f, sampleRate * channels * 2);
  wr16(_f, channels * 2);
  wr16(_f, 16);
  _f.write((const uint8_t *)"data", 4);  wr32(_f, preBytes);

  _open = true;
  return true;
}

bool WavWriter::writeSamples(const int16_t *src, uint32_t n) {
  if (!_open) return false;
  size_t w = _f.write((const uint8_t *)src, n * 2);
  _dataBytes += (uint32_t)w;
  return w == n * 2;
}

// ---------------------------------------------------------------------------
//  Patch the length while the recording is still running
//
//  What open() writes into the header is an "estimated length" (StereoCapture
//  has no estimate and writes 0); the real length used to be patched only in
//  close(). The trouble is that this leaves a failure mode that is very hard to
//  track down: power loss, a reset press, or the SD card pulled out before the
//  performance/recording ends -- several MB of audio are already written, but
//  the data length in the header is still 0.
//
//  Such a file behaves differently in different players, and that is exactly
//  what makes it hard to diagnose:
//    Windows Explorer / Media Player  -> calls it corrupt, refuses to play it
//    ffmpeg / QuickTime / Audacity    -> scan to the end themselves, play fine
//  Hence "it plays fine on the Mac, but Windows says the file is broken".
//
//  The fix is to seek back periodically during recording, patch the length to
//  "so far" and flush. The cost is one seek plus 8 bytes plus one flush per
//  time; recorder.cpp does it every 2 seconds, negligible against the 176 KB/s
//  write rate. On power loss at most the last 2 seconds are lost.
// ---------------------------------------------------------------------------
void WavWriter::flushHeader() {
  if (!_open) return;
  const uint32_t pos = _f.position();      // remember how far we have written
  _f.seek(4);   wr32(_f, 36 + _dataBytes);
  _f.seek(40);  wr32(_f, _dataBytes);
  _f.seek(pos);                            // must go back to where we were, or the following writes overwrite ourselves
  _f.flush();                              // without the flush the patched bytes sit in cache and are lost on power failure anyway
}

void WavWriter::close() {
  if (!_open) return;
  flushHeader();
  _f.close();
  _open = false;
}

// ---------------------------------------------------------------------------
//  What an audio file generated by the program itself looks like
//
//  Only two forms are recognised:
//    1) fixed names  REC.WAV (microphone recording), PLAY.WAV (chromatic scale
//       output), CANON.WAV (canon output). CANON.WAV was once the old name of
//       PLAY.WAV, and now that the canon score is back it is a live name again
//       -- both cases have to be recognised
//    2) bare note names  C4.WAV, Db4.WAV, A3.WAV … (auto-named by sampling mode)
//
//  Material the user dropped in themselves (Piano.mf.C4.wav,
//  Trumpet.vib.ff.C4.stereo.wav) all carries a prefix and never falls into form
//  2, so it can never be deleted. That matters a lot -- deleting someone's
//  painstakingly recorded material by mistake is irreversible, so keep the rule
//  strict and miss a few files instead.
// ---------------------------------------------------------------------------
bool tcIsGeneratedWav(const char *nm) {
  if (!nm) return false;

  // The extension must be .WAV (case-insensitive)
  const size_t len = strlen(nm);
  if (len < 5) return false;
  if (strcasecmp(nm + len - 4, ".WAV") != 0) return false;

  if (strcasecmp(nm, TC_REC_PATH) == 0 || strcasecmp(nm, "REC.WAV") == 0)   return true;
  if (strcasecmp(nm, TC_PLAY_PATH) == 0 || strcasecmp(nm, TC_CANON_PATH) == 0) return true;
  if (strcasecmp(nm, "CANON.WAV") == 0) return true;   // the old name on older cards

  // Bare note name: a letter A~G, optionally one b or #, then 1~2 digits, then the end
  size_t i = 0;
  const char c = (char)toupper((unsigned char)nm[i]);
  if (c < 'A' || c > 'G') return false;
  i++;
  if (nm[i] == 'b' || nm[i] == 'B' || nm[i] == '#') i++;
  size_t digits = 0;
  while (i < len - 4 && nm[i] >= '0' && nm[i] <= '9') { i++; digits++; }
  return digits >= 1 && digits <= 2 && i == len - 4;
}

// ============================================================================
//  Sampling folders SETnn
// ============================================================================
bool tcIsSetDir(const char *nm) {
  if (!nm) return false;
  const size_t pre = strlen(TC_SET_PREFIX);
  if (strlen(nm) != pre + 2) return false;
  if (strncasecmp(nm, TC_SET_PREFIX, pre) != 0) return false;
  return nm[pre] >= '0' && nm[pre] <= '9' && nm[pre + 1] >= '0' && nm[pre + 1] <= '9';
}

bool tcSdMakeNextSet(char *out, size_t cap) {
  // Probe one at a time with SD.exists(); do not scan the directory.
  //
  // Scanning and taking the maximum looks smarter, but it has a hole: once the
  // user manually deletes SET02 the maximum is still SET03, the next one handed
  // out is SET04 -- and the number SET02 stays empty forever. Probing in order
  // fills SET02 back in. Consecutive numbers are easier for people to read.
  for (int i = 1; i <= TC_SET_MAX; i++) {
    char name[12];
    snprintf(name, sizeof(name), "%s%02d", TC_SET_PREFIX, i);
    if (SD.exists(name)) continue;
    if (!SD.mkdir(name)) {
      Serial.printf("[SD] 建立資料夾 %s 失敗\n", name);
      return false;
    }
    snprintf(out, cap, "%s", name);
    return true;
  }
  Serial.printf("[SD] %s01~%s%02d 都被用掉了，先刪幾組再來\n",
                TC_SET_PREFIX, TC_SET_PREFIX, TC_SET_MAX);
  return false;
}

int tcSdCollectSets(char *outNames, int maxCount) {
  File d = SD.open("/");
  if (!d) return 0;
  int n = 0;
  while (n < maxCount) {
    File e = d.openNextFile();
    if (!e) break;
    const char *nm = e.name();
    if (nm && e.isDirectory() && tcIsSetDir(nm))
      snprintf(outNames + (size_t)n++ * TC_MAX_NAME_LEN, TC_MAX_NAME_LEN, "%s", nm);
    e.close();
  }
  d.close();

  // sort, so the menu order is the same every time
  for (int i = 1; i < n; i++) {
    char key[TC_MAX_NAME_LEN];
    snprintf(key, TC_MAX_NAME_LEN, "%s", outNames + (size_t)i * TC_MAX_NAME_LEN);
    int j = i - 1;
    while (j >= 0 && strcmp(outNames + (size_t)j * TC_MAX_NAME_LEN, key) > 0) {
      memcpy(outNames + (size_t)(j + 1) * TC_MAX_NAME_LEN,
             outNames + (size_t)j * TC_MAX_NAME_LEN, TC_MAX_NAME_LEN);
      j--;
    }
    memcpy(outNames + (size_t)(j + 1) * TC_MAX_NAME_LEN, key, TC_MAX_NAME_LEN);
  }
  return n;
}

bool tcSdRemoveDir(const char *dir, int *deletedFiles, uint32_t *freedBytes) {
  if (deletedFiles) *deletedFiles = 0;
  if (freedBytes)   *freedBytes   = 0;
  if (!dir || !dir[0]) return false;

  // Scan once to collect the names first, then delete. Deleting directory
  // entries while iterating over them is undefined on FAT and in practice
  // misses half the files -- purgeGeneratedWavs() already fell into this hole.
  static char victims[TC_MAX_SCAN_FILES][TC_MAX_NAME_LEN];
  int nv = 0;
  uint32_t freed = 0;

  File d = SD.open(dir);
  if (!d) { Serial.printf("[SD] 打不開 %s\n", dir); return false; }
  if (!d.isDirectory()) { d.close(); Serial.printf("[SD] %s 不是資料夾\n", dir); return false; }
  while (nv < TC_MAX_SCAN_FILES) {
    File e = d.openNextFile();
    if (!e) break;
    const char *nm = e.name();
    if (nm && nm[0] != '.' && !e.isDirectory()) {
      snprintf(victims[nv], TC_MAX_NAME_LEN, "%s", nm);
      freed += e.size();
      nv++;
    }
    e.close();
  }
  d.close();

  int done = 0;
  for (int i = 0; i < nv; i++) {
    char full[TC_MAX_NAME_LEN * 2 + 2];
    snprintf(full, sizeof(full), "%s/%s", dir, victims[i]);
    if (SD.remove(full)) done++;
    else Serial.printf("[SD] 刪不掉 %s\n", full);
  }

  const bool ok = SD.rmdir(dir);
  if (!ok) Serial.printf("[SD] 資料夾 %s 刪不掉（裡面還有東西？）\n", dir);

  if (deletedFiles) *deletedFiles = done;
  if (freedBytes)   *freedBytes   = freed;
  return ok && done == nv;
}

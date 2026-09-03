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
    if (nm && nm[0] != '.') {                       // 跳過 macOS 的隱藏檔
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
      // 檔名太長就整個跳過，不要截斷 —— 截斷後之後開檔一定失敗，
      // 而且會變成很難查的「檔案明明在卻找不到」。
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

  // 依檔名排序，讓多次執行的載入順序一致（訓練結果才可重現）
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

// ------------------------------------------------------------------ 小工具 --
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

// ==================================================================== 讀取 ==
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
      else                s = ((int32_t)buf[i * 2] + (int32_t)buf[i * 2 + 1]) / 2;   // 混成 mono
      dst[done + i] = (float)s * (1.0f / 32768.0f);
    }
    done += gotFrames;
    if (gotFrames < k) break;
  }
  for (uint32_t i = done; i < n; i++) dst[i] = 0.0f;      // 補零
  return done;
}

// ==================================================================== 寫入 ==
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

  const uint32_t preBytes = expectedSamples * 2u;   // 先寫入的預估 data 長度
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
//  錄製途中就把長度補正
//
//  標頭在 open() 時寫的是「預估長度」（StereoCapture 沒有預估值，寫的是 0），
//  真正的長度本來只在 close() 補。問題是這樣有一個很難查的失敗模式：
//  演奏／錄音還沒結束就斷電、按 reset、或直接把 SD 卡拔起來 —— 音訊資料
//  明明已經寫進去好幾 MB，但標頭裡的 data 長度還是 0。
//
//  這種檔案在不同播放器上的行為不一樣，而那正是它難查的地方：
//    Windows 檔案總管 / Media Player  -> 判定損毀，不給播
//    ffmpeg / QuickTime / Audacity    -> 會自己掃到檔尾，照樣播得出來
//  於是「在 Mac 上明明聽得到，拿到 Windows 就說檔案壞掉」。
//
//  解法是錄製途中定期回頭把長度補到「目前為止」並 flush。成本是每次一個
//  seek 加 8 個位元組再加一次 flush；recorder.cpp 每 2 秒做一次，
//  相對於 176 KB/s 的寫入量可以忽略。斷電的話最多只損失最後 2 秒。
// ---------------------------------------------------------------------------
void WavWriter::flushHeader() {
  if (!_open) return;
  const uint32_t pos = _f.position();      // 記住目前寫到哪
  _f.seek(4);   wr32(_f, 36 + _dataBytes);
  _f.seek(40);  wr32(_f, _dataBytes);
  _f.seek(pos);                            // 一定要回到原位，否則接下來會蓋掉自己
  _f.flush();                              // 不 flush 的話補正的位元組還在快取裡，斷電照樣丟失
}

void WavWriter::close() {
  if (!_open) return;
  flushHeader();
  _f.close();
  _open = false;
}

// ---------------------------------------------------------------------------
//  程式自己產生的音檔長什麼樣子
//
//  只認兩種：
//    1) 固定檔名  REC.WAV（麥克風錄音）、PLAY.WAV（半音階成品）、
//       CANON.WAV（卡農成品）。CANON.WAV 一度是 PLAY.WAV 的舊名，
//       卡農樂譜接回來之後它又是現役檔名了 —— 兩種情況都要認得
//    2) 純音名    C4.WAV、Db4.WAV、A3.WAV …（採樣模式自動命名的）
//
//  使用者自己丟進去的素材（Piano.mf.C4.wav、Trumpet.vib.ff.C4.stereo.wav）
//  都有前綴，不會落進第 2 類，所以刪不到。這一點很重要 ——
//  誤刪別人辛苦錄的素材是不可逆的，寧可規則嚴一點、漏刪幾個檔。
// ---------------------------------------------------------------------------
bool tcIsGeneratedWav(const char *nm) {
  if (!nm) return false;

  // 副檔名必須是 .WAV（不分大小寫）
  const size_t len = strlen(nm);
  if (len < 5) return false;
  if (strcasecmp(nm + len - 4, ".WAV") != 0) return false;

  if (strcasecmp(nm, TC_REC_PATH) == 0 || strcasecmp(nm, "REC.WAV") == 0)   return true;
  if (strcasecmp(nm, TC_PLAY_PATH) == 0 || strcasecmp(nm, TC_CANON_PATH) == 0) return true;
  if (strcasecmp(nm, "CANON.WAV") == 0) return true;   // 舊卡上的舊檔名

  // 純音名：字母 A~G，可接一個 b 或 #，再接 1~2 位數字，然後就結束
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
//  採樣資料夾 SETnn
// ============================================================================
bool tcIsSetDir(const char *nm) {
  if (!nm) return false;
  const size_t pre = strlen(TC_SET_PREFIX);
  if (strlen(nm) != pre + 2) return false;
  if (strncasecmp(nm, TC_SET_PREFIX, pre) != 0) return false;
  return nm[pre] >= '0' && nm[pre] <= '9' && nm[pre + 1] >= '0' && nm[pre + 1] <= '9';
}

bool tcSdMakeNextSet(char *out, size_t cap) {
  // 用 SD.exists() 一個一個試，不掃目錄。
  //
  // 掃目錄再取最大值看起來比較聰明，但那樣有個洞：使用者手動刪掉 SET02 之後，
  // 最大值仍然是 SET03，下一個會給 SET04 —— SET02 那個號碼就永遠空著。
  // 逐一試就會補進去 SET02。號碼連續，人比較好認。
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

  // 排序，讓選單每次的順序一致
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

  // 先掃一遍收集檔名再刪。邊迭代邊刪目錄項目在 FAT 上行為未定義，
  // 實務上會漏掉一半的檔案 —— purgeGeneratedWavs() 已經踩過這個坑。
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

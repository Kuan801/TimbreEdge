// ============================================================================
//  wav_io.h  -  極簡 16-bit PCM WAV 讀寫 (支援 mono / stereo，讀取時自動降成 mono)
// ============================================================================
#pragma once

#include <Arduino.h>
#include <SD.h>
#include "config.h"

// SD 初始化：先試 Teensy 4.1 內建 SDIO，失敗再試 Audio Shield 的 SPI 插槽
bool tcSdBegin();

// 列出根目錄內容。診斷用：可以立刻分辨「SD 沒掛起來」和「檔案不在卡上」。
void tcSdList();

// 掃描某個目錄底下所有 .WAV，把檔名收集到 outNames（每筆 TC_MAX_NAME_LEN bytes）。
// dir 傳 "/" 代表根目錄。回傳找到幾個。
// skipName 非 NULL 時會跳過該檔（例如批次載入時排除剛錄的 REC.WAV）。
int tcSdCollectWavs(const char *dir, char *outNames, int maxCount,
                    const char *skipName = nullptr);

// 複製檔案（採樣模式要把 REC.WAV 另存成 A4.WAV 這種以音高命名的檔）
bool tcSdCopy(const char *src, const char *dst);

// ------------------------------------------------------- 採樣資料夾（SETnn）--
//
// 每次連續採樣都開一個新資料夾（SET01、SET02…），那一輪的音檔全部收在裡面。
//
// 為什麼要分：以前所有音檔都堆在根目錄，第二次採樣的 C4.WAV 會直接蓋掉第一次的，
// 而且「n *」會把不同樂器、不同輪次的素材全部混在一起訓練，從外面完全看不出來。
// 分資料夾之後，一輪就是一組，要用哪一組就 "n SET01/"。
//
// 為什麼用流水號而不是時間戳記：Teensy 4.1 的 RTC 要裝鈕扣電池才會走。
// 沒電池的話每次開機都從同一個時間開始，時間戳記反而會互相覆蓋 ——
// 比流水號更糟。流水號只依賴「卡上已經有哪些資料夾」，永遠正確。

#define TC_SET_PREFIX      "SET"
#define TC_SET_MAX         99

// 找出下一個沒用過的 SETnn 並建立它。成功時把名字寫進 out（例如 "SET03"）。
// 卡上已經有 SET01~SET99 就回 false。
bool tcSdMakeNextSet(char *out, size_t cap);

// 這個名字是不是採樣資料夾（SET 開頭 + 兩位數字）。
// 純字串判斷，桌機測得到。
bool tcIsSetDir(const char *name);

// 掃出卡上所有採樣資料夾，依名稱排序。回傳找到幾個。
int tcSdCollectSets(char *outNames, int maxCount);

// 刪掉一整個資料夾（先刪裡面的檔，再刪目錄本身）。
// deletedFiles / freedBytes 非 NULL 時回報刪了幾個檔、釋放多少空間。
bool tcSdRemoveDir(const char *dir, int *deletedFiles = nullptr,
                   uint32_t *freedBytes = nullptr);

// ------------------------------------------------------------------ 讀取器 --
class WavReader {
public:
  bool     open(const char *path);
  void     close();
  bool     isOpen() const { return _open; }

  uint32_t frames()     const { return _frames; }      // 每聲道的取樣點數
  uint32_t sampleRate() const { return _sampleRate; }
  uint16_t channels()   const { return _channels; }

  // 從第 frameIndex 個取樣點開始讀 n 點 (mono float, -1..1)。回傳實際讀到的點數。
  uint32_t readMono(uint32_t frameIndex, float *dst, uint32_t n);

private:
  File     _f;
  bool     _open       = false;
  uint32_t _dataOffset = 0;
  uint32_t _frames     = 0;
  uint32_t _sampleRate = 44100;
  uint16_t _channels   = 1;
  uint16_t _bits       = 16;
};

// ------------------------------------------------------------------ 寫入器 --
class WavWriter {
public:
  // expectedSamples > 0 時會先把正確長度寫進標頭。
  // 這樣即使某些 SD 實作不允許回頭 seek 改標頭，檔案仍然是合法的 WAV。
  bool open(const char *path, uint32_t sampleRate = 44100, uint16_t channels = 1,
            uint32_t expectedSamples = 0);
  bool writeSamples(const int16_t *src, uint32_t n);   // n = int16 個數
  void close();                                        // 回頭補正 RIFF 長度
  // 錄製途中就把 RIFF/data 長度補到目前為止的正確值，然後 flush 到卡上。
  // 為什麼需要：長度只在 close() 補的話，「還沒按停就斷電／重開機／拔卡」
  // 留下的檔案 data 長度是 0 —— Windows 直接判定損毀不給播，即使裡面
  // 已經有好幾 MB 的音訊。詳見 wav_io.cpp 的說明。
  void flushHeader();
  bool isOpen() const { return _open; }
  uint32_t bytesWritten() const { return _dataBytes; }

private:
  File     _f;
  bool     _open      = false;
  uint32_t _dataBytes = 0;
  uint32_t _sampleRate = 44100;
  uint16_t _channels   = 1;
};

// 這個檔名是不是「程式自己產生的音檔」（REC.WAV / PLAY.WAV / 純音名檔）。
//
// 拉出來放這裡而不是留在 .ino，是因為它是刪檔用的判斷 —— 誤刪使用者辛苦錄的
// 素材不可逆，這種規則必須測得到。見 tools/sim/wavname_test.cpp。
bool tcIsGeneratedWav(const char *name);

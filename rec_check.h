// ============================================================================
//  rec_check.h  -  「這次單音錄音到底行不行」的判定
//
//  為什麼要獨立一個檔案：判定本身是純粹的數值邏輯，跟 SD、OLED、Arduino
//  完全無關。拆出來就能在桌機上把每一條規則跑過一遍（tools/sim/reccheck），
//  不用每改一個門檻就燒錄一次。
//
//  設計原則：只講「哪裡不對」跟「怎麼改」，不講原理。站在機器前面手上還拿著
//  樂器的人，需要的是下一步要按哪個鍵，不是一份診斷報告。
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

enum RecVerdict : uint8_t {
  REC_OK   = 0,   // 可以用
  REC_WARN = 1,   // 能用，但有明顯可以改善的地方
  REC_BAD  = 2,   // 不要用，重錄
};

struct RecCheck {
  bool  analysisOk;     // analyzeWavFile() 有沒有成功
  float peak;           // 全檔絕對峰值 0..1
  float clipRatio;      // 削波樣本佔比
  float noiseFloor;     // 起音前的平均 RMS（相對峰值）
  int   onsets;         // 起音次數，1 才是單音
  float noteDur;        // 有效音長（秒）
  float f0;             // 基頻 Hz
  float decayPerSec;    // 持續段每秒衰減倍率
};

// reason / fix 各一行，長度以 OLED 的 21 個字元為準（超過會被截斷）。
// 兩個都可以傳 nullptr。
RecVerdict recCheckEval(const RecCheck &in,
                        char *reason, size_t reasonCap,
                        char *fix,    size_t fixCap);

// 給面板用的短標題："REC OK" / "REC - CHECK THIS" / "REC FAILED"
const char *recVerdictTitle(RecVerdict v);

// 訊噪比（dB）。noiseFloor 是相對峰值的線性比例。
//
// 量不到的情況是真的會發生的：連續採樣模式有 93 ms 的前置緩衝，音一觸發就
// 從緩衝的最前面開始寫檔，起音正好落在第 0 格，前面沒有任何一格可以當底噪。
// 那時候回 RECCHK_SNR_UNKNOWN，判定要跳過所有跟訊噪比有關的規則 ——
// 「量不到」跟「很乾淨」是兩件事，不能拿來當作品質良好的證據。
#define RECCHK_SNR_UNKNOWN  (-999.0f)
float recCheckSnrDb(float noiseFloor);
bool  recCheckSnrKnown(float noiseFloor);

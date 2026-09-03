// ============================================================================
//  analyzer.h  -  離線(非即時)分析一段單音 WAV，產出 InstrumentProfile
//
//  流程：
//    1. 掃一次全檔算 RMS 包絡  -> 找 onset / offset / ADSR
//    2. 在最穩定的幾格用 YIN 抓基頻 f0（取中位數）
//    3. 逐格 FFT，在 h*f0 附近做拋物線內插取諧波振幅
//    4. 把所有格壓縮成 TC_N_KEYFRAME 個關鍵影格
//    5. 從最大音量那格萃取 log 頻譜包絡（共振峰）
//    6. 用「總能量 - 諧波能量」估非諧波噪聲比
//
//  這一段是阻塞式的，跑在 loop() 裡，大約 2~6 秒（5 秒素材）。
//  期間 audio ISR 照常運作，不會爆音。
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "profile.h"

class TrainSet;      // trainer.h

// 分析是阻塞式的（1~3 秒），設一個進度回呼讓 OLED 不會看起來像當機。
// frac 為 0..1。傳 nullptr 取消。
void analyzerSetProgressCallback(void (*cb)(float frac));

// 最近一次分析在錄音裡數到幾次起音。1 = 正常的單音。
//
// >= 2 代表這段錄音裡撥/彈了不只一次，整份 profile 都不可信：ADSR、衰減速率、
// shimmer、噪聲比全都建立在「單音」的假設上。實測吉他錄了 6 次撥弦時，
// 衰減會從真值 0.5 變成 0.95（合成出來像管風琴一樣一直響）。
//
// 面板要看得到 —— 手上拿著樂器的人不會在看序列埠。
int analyzerLastOnsetCount();

// 最近一次分析量到的錄音品質。合成聽起來不對的時候，八成問題在這三個數字，
// 而不是在合成器：
//   Peak       全檔絕對峰值。< 0.05 太小聲（量化雜訊比重過高），
//              1.0 代表已經削波（波形被削平，諧波全是假的）。
//   ClipRatio  削波樣本佔比。> 0.001 就該調低 micGain。
//   NoiseFloor 起音之前的平均 RMS（相對峰值）。> 0.1（訊噪比 < 20 dB）
//              代表觸發被環境噪音搶先，真正的音只佔錄音的一小段。
float analyzerLastPeak();
float analyzerLastClipRatio();
float analyzerLastNoiseFloor();

// csvDumpPath : 非 NULL 時把每格結果 dump 成 CSV（給電腦端訓練用）
// trainSet    : 非 NULL 時把每格結果直接堆進訓練集（機上訓練用）
//               兩者的特徵與目標算法完全一致，所以兩條路訓練出的模型等價。
bool analyzeWavFile(const char *wavPath,
                    InstrumentProfile &out,
                    const char *csvDumpPath = nullptr,
                    TrainSet *trainSet = nullptr);

// ============================================================================
//  trigger.h  -  連續採樣的「什麼時候該開始錄」判斷
//
//  獨立成一個檔案的理由跟 rec_check 一樣：這裡面沒有一行跟 SD、OLED、
//  Arduino 有關，純粹是「餵進 block 峰值、吐出要不要觸發」的狀態機。
//  拆出來就能在桌機上餵合成訊號跑過每一條路徑（tools/sim/trigger_test）。
//
//  --- 為什麼要重寫 ---------------------------------------------------------
//
//  舊版用固定門檻 TC_TRIG_LEVEL = 0.035，在沒有人演奏的情況下會不停觸發。
//  量了實際錄到的檔案才知道餘裕有多小：
//
//      背景噪音的 block 峰值   中位 0.021   99% 0.028   最大 0.030
//      最小聲的那個音          block 峰值中位 0.053
//      固定門檻                0.035
//
//  背景最大值離門檻只有 1.16 倍（1.3 dB），最弱的音離背景最大值只有 1.76 倍
//  （4.9 dB）。這種餘裕下，房間裡多一台風扇就會整晚亂錄。
//
//  所以改成兩件事：
//    1) 門檻不再寫死，而是「量到的環境噪音 × 邊界」——房間吵就自己抬高。
//    2) 觸發前必須先看到一段真正的安靜。舊版這個保護寫反了（見下面）。
//
//  --- 舊版那個寫反的保護 -----------------------------------------------------
//
//      if (pk < _thresh * 0.5f) _quietSince = millis();
//      ...
//      if (++_hotBlocks >= N && (millis() - _quietSince) > TC_REARM_SILENT_MS)
//
//  本意是「錄完之後要先安靜一段時間才重新武裝」，但寫出來的意思是
//  「最後一次看到安靜是在 400 ms 以前」。背景只要一直高於半個門檻，
//  _quietSince 就永遠不更新，這個差值只會越來越大 ——
//  也就是說，房間越吵，這個保護越不會擋，正好跟本意相反。
//
//  現在改成閂鎖：必須「連續」安靜滿 TC_REARM_SILENT_MS 才把 _rearmed 設起來，
//  觸發時消耗掉。持續的噪音永遠湊不滿那段連續安靜，所以永遠不會武裝。
// ============================================================================
#pragma once

#include <stdint.h>
#include "config.h"

// ---------------------------------------------------------------------------
//  一個 audio block 的「電平」：第 TC_BLOCK_TOPK 大的取樣絕對值
//
//  以前直接取最大值。問題是最大值對「單一取樣的脈衝」毫無抵抗力，而實機上
//  那種脈衝多得很（數位耦合）：某台機器安靜時 block RMS 只有 0.002，
//  卻有 4~8% 的 block 帶著一根 0.19~0.28 的針。用最大值去看，這些 block
//  跟真的有人在彈是一模一樣的。
//
//  取第 4 大就分得開了，而且**單位不變**：
//    真實樂音   128 個取樣裡有幾十個都貼近峰值（440 Hz 的 block 有 1.3 個
//               週期，|sin| > 0.9 的取樣佔 29%），第 4 大 ≈ 最大值。
//    單根脈衝   只有 1~3 個取樣是大的，第 4 大直接落回底噪。
//
//  單位不變很重要：TC_TRIG_LEVEL、trigger_test 裡那些門檻數字全都是拿
//  真實錄音的「block 峰值」量出來的，換一個尺度（例如改用 block RMS）
//  就得整批重新校準，而且 README 上那些量測值會全部失效。
//
//  放在這裡而不是 recorder.cpp：這是「什麼時候該開始錄」的一部分，
//  而且是純函式 —— 放這裡桌機測得到（tools/sim/trigger_test）。
// ---------------------------------------------------------------------------
#define TC_BLOCK_TOPK 4
float tcBlockLevel(const int16_t *src, int n);

class TriggerGate {
public:
  // baseThreshold：使用者設定的下限（`s 0.01` 或 TC_TRIG_LEVEL）。
  // 實際生效的門檻不會低於它，但可以被環境噪音抬高。
  void arm(float baseThreshold, uint32_t nowMs);

  // 餵一個 audio block 的峰值（0..1）。回傳 true 代表「現在開始錄」。
  bool feed(float blockPeak, uint32_t nowMs);

  // 錄完一段之後呼叫：把武裝狀態清掉，強迫重新等一段安靜。
  // 不做這件事的話，音的尾巴會馬上再觸發一次。
  void noteRecorded(uint32_t nowMs);

  bool  calibrating() const { return _calibrating; }
  float ambient()     const { return _ambient; }     // 量到的環境噪音峰值
  float threshold()   const { return _thresh;  }     // 實際生效的門檻
  float level()       const { return _level;   }     // 平滑後的電平，給 UI
  bool  armedReady()  const { return _rearmed; }

  // 校正期間看到幾個「遠高於環境」的孤立尖峰。
  // 用高百分位估環境之後，這些尖峰不再會毀掉門檻，但它們仍然是硬體問題
  // 的徵兆（數位耦合、接地），該讓使用者知道 —— 不要靜靜地容忍它。
  int   calSpikes()   const { return _calSpikes; }

  // 上一次觸發時，訊號比環境噪音高多少 dB。
  // 這是「這個環境到底能不能用」最直接的一個數字，低於
  // TC_TRIG_MIN_HEADROOM_DB 就該提醒使用者，而不是默默錄一堆廢檔。
  float lastHeadroomDb() const;

  // 目前環境下的門檻離使用者設定的下限差多少倍。> 1 代表是噪音在主導。
  float ambientRatio() const { return _base > 0 ? _thresh / _base : 1.0f; }

private:
  // 校正期間保留「最大的幾個 block 峰值」，最後取第 CAL_KEEP 大的當環境噪音。
  //
  // 原本取最大值，理由是「會誤觸發的是尖峰，不是平均值」，而且實測乾淨
  // 背景的 峰值/中位數 只有 1.46，用最大值不會過度保守。那個推論在乾淨
  // 背景下成立，但最大值這個估計量的崩潰點是 0 —— **一個離群值就毀掉它**。
  //
  // 實機上真的發生了：某台機器的輸入偶爾會有孤立的數位尖峰（交流 RMS 只有
  // 0.001，尖峰卻到 0.28）。校正的 600 ms 內只要撞上一個，環境就被量成
  // 0.2767，門檻 = 0.2767 × 1.5 = 0.415，之後怎麼彈都不可能觸發，
  // 而畫面上只會說「等待中… 電平 0.06 / 門檻 0.41」。
  //
  // 600 ms 約 207 個 block，取第 8 大 ≈ 96 百分位：可以吃掉 7 個尖峰，
  // 而乾淨背景下它跟最大值幾乎沒有差別（實測背景 99% 0.028、最大 0.030）。
  // 跟 analyzer 量 noiseGain 時「取 5 個點的中位數」是同一個理由。
  static const int CAL_KEEP = 8;
  float    _calTop[CAL_KEEP];
  int      _calN       = 0;
  int      _calSpikes  = 0;

  float    _base       = TC_TRIG_LEVEL;
  float    _ambient    = 0.0f;
  float    _thresh     = TC_TRIG_LEVEL;
  float    _level      = 0.0f;
  float    _lastTrigPk = 0.0f;
  float    _lastAmb    = 0.0f;
  bool     _calibrating = false;
  bool     _rearmed     = false;
  bool     _quiet       = false;
  uint32_t _calUntil    = 0;
  uint32_t _quietSince  = 0;
  int      _hot         = 0;

  void  recomputeThreshold();
  float quietLevel() const;
};

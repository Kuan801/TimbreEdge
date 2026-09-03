// ============================================================================
//  trainer.h  -  在 Teensy 上直接訓練那顆 MLP
//
//  跟 tools/train_ddsp.py 是同一套數學，只是改用 C++ 手刻：
//    損失 = 交叉熵(32 個諧波的 softmax) + 0.3 * BCE(噪聲的 sigmoid)
//    最佳化 = Adam
//
//  為什麼可以在 MCU 上訓練：
//    每筆樣本前向 2208 + 反向 4288 ≈ 6.5k MAC。batch 128 × 6000 epoch
//    = 768k 次樣本傳遞 ≈ 5.0 G MAC。Cortex-M7 @600MHz 帶單週期 FMA，
//    實測落在 20~40 秒。
//    記憶體：訓練資料 215 KB + Adam/梯度 28 KB + 權重 9 KB ≈ 252 KB，
//    Teensy 4.1 的 OCRAM 有 512 KB，塞得下。
//
//  訓練期間 audio ISR 照常運作（它是中斷，優先權比 loop() 高），不會爆音，
//  只是那幾十秒不能演奏。
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "timbre_model.h"

// 一筆訓練樣本 = 84 bytes。
//
// 諧波目標用「平方根壓伸 + int16」而不是 float32：
//   32 個 float 要 128 bytes，訓練集會膨脹到 379 KB 塞不下；
//   而平方根壓伸後，小振幅（高次諧波常低到 0.001）的相對解析度是 0.1%，
//   反而比 float32 直接截成 int16 好一個數量級。
//   量化雜訊遠低於實測的 0.0002 收斂誤差，不影響訓練品質。
#define TC_Q_SCALE 32767.0f
static inline int16_t tc_quant(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 1.0f) return (int16_t)TC_Q_SCALE;
  return (int16_t)(sqrtf(v) * TC_Q_SCALE + 0.5f);
}
static inline float tc_dequant(int16_t q) {
  float x = (float)q * (1.0f / TC_Q_SCALE);
  return x * x;
}

struct TrainSample {
  float   in[TC_MLP_IN];        // 16 bytes
  int16_t harm[TC_N_HARM];      // 64 bytes，已正規化（還原後總和 ≈ 1）
  int16_t noise;                //  2 bytes
  int16_t _pad;                 //  2 bytes（保持 4-byte 對齊）
};

class TrainSet {
public:
  void clear();
  bool add(const float *in, const float *harm, float noise);   // 滿了回 false

  int  size()  const { return _n; }

  // 退回到某個大小。連續採樣要用：分析跟「加進訓練集」是同一趟做的，
  // 等到判定出爐才知道這個音不能用 —— 那時候樣本已經進去了，得能收回來。
  // 只往回不往前，所以不會憑空造出資料。
  void truncate(int n) { if (n >= 0 && n < _n) _n = n; }
  bool full()  const { return _n >= TC_TRAIN_MAX; }
  const TrainSample *data() const;

  // 印出目前累積了什麼（幾筆、涵蓋哪些音高）
  void summary() const;
  // 涵蓋幾個不同音高（用輸入特徵 in[0] 分群）
  int  pitchCount() const;

private:
  int  _n      = 0;
  bool _warned = false;
};

// 訓練是阻塞式的（幾十秒），設進度回呼讓 OLED 能即時顯示收斂狀況。
// 傳 nullptr 取消。
void trainerSetProgressCallback(void (*cb)(int epoch, int total, float ce, float mae));

// 訓練。回傳 false 表示資料不足或發散。
//   progressEvery: 每幾個 epoch 回報一次進度 (0 = 不回報)
bool trainMlp(const TrainSet &ts, MlpWeights &out,
              int epochs = TC_TRAIN_EPOCHS,
              float lr = TC_TRAIN_LR,
              uint32_t seed = 12345,
              int progressEvery = 500);

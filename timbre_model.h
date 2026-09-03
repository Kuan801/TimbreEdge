// ============================================================================
//  timbre_model.h  -  DDSP-lite 音色模型
//
//  這是「機器學習」的部分，但刻意做得很小，因為它每 2.9 ms 就要為每個發聲的
//  音跑一次推論：
//
//      輸入 (4):  [ 音高(log), 響度, 起音後經過的正規化時間, 是否已放開 ]
//      隱藏:      32 -> 32   (tanh)
//      輸出 (33): 32 個諧波權重 (softmax) + 1 個噪聲增益 (sigmoid)
//
//  參數量 2305 個 float ≈ 9.0 KB（權重 2208 + 偏移 97），單次推論約 2.2k MAC。
//  6 個複音 × 每秒 344 個 block ≈ 4.6 M MAC/s（8 複音 6.1 M），
//  Teensy 4.1 (600 MHz, FPU) 佔用不到 2% CPU。
//
//  ★ 沒有 MODEL.BIN 時會自動退回「關鍵影格內插」，音色一樣可用，只是少了
//    對響度/音高的非線性適應。
//
//  ★ 不論走哪條路，最後都會套上「頻譜包絡(共振峰)校正」：
//    移調時保持共振峰位置不動，這是讓合成音在整個卡農音域裡不會變成
//    花栗鼠的關鍵。
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "profile.h"

struct MlpWeights {
  uint32_t magic;
  float w1[TC_MLP_H1][TC_MLP_IN];
  float b1[TC_MLP_H1];
  float w2[TC_MLP_H2][TC_MLP_H1];
  float b2[TC_MLP_H2];
  float w3[TC_MLP_OUT][TC_MLP_H2];
  float b3[TC_MLP_OUT];
};

class TimbreModel {
public:
  void setProfile(const InstrumentProfile *p) { _p = p; }
  // 音色庫：每個音符各自挑最接近的取樣點，把移調距離壓到半音以內
  void setBank(const ProfileBank *b) { _bank = b; }
  const InstrumentProfile *profileFor(float f0Play) const {
    if (_bank && _bank->n > 0) { const InstrumentProfile *q = _bank->get(f0Play); if (q) return q; }
    return _p;
  }
  bool loadWeights(const char *path = TC_MODEL_PATH);
  void unloadWeights() { _hasMlp = false; }

  // 機上訓練完直接套用（不必先寫檔再讀回來）
  void adoptWeights(const MlpWeights &w);
  // 把目前的權重存成 MODEL.BIN，格式與 Python 訓練出來的完全相同
  bool saveWeights(const MlpWeights &w, const char *path = TC_MODEL_PATH) const;

  bool hasMlp()   const { return _hasMlp; }

  // MLP 在最終音色裡的權重。編譯期預設值是 TC_MLP_BLEND，執行期可以改 ——
  // 序列埠 'k' 就是靠這個做現場 A/B，不必重新編譯。
  void  setBlend(float b) { _blend = tc_clampf(b, 0.0f, 1.0f); }
  float blend() const { return _blend; }

  // 「MLP 這一刻真的有在影響聲音嗎」。權重載入了但 blend 是 0 的話答案是否 ——
  // 顯示狀態要用這個，不是 hasMlp()，否則畫面會宣稱 MLP 有作用而其實沒有。
  bool  mlpActive() const { return _hasMlp && _blend > 0.0f; }
  bool ready()    const { return (_bank && _bank->n > 0) || (_p && _p->valid); }
  const InstrumentProfile *profile() const { return _p; }

  // 產生某個瞬間的諧波振幅。
  //   f0Play  : 要演奏的基頻 (Hz)
  //   loud    : 0..1 目前的包絡響度（含力度）
  //   tNorm   : 0..1 對數時間軸上的位置（用 tc_timeWarp 算）
  //   released: 是否已進入 release
  //   ampOut  : 長度 nPartials，回傳「線性振幅」，已含頻譜包絡校正與抗混疊。
  //             前 TC_N_HARM 根來自模型，其餘由頻譜包絡外推。
  //   noiseOut: 噪聲層增益
  //   nPartials: 這個音高要發幾根（用 tc_partialCount 算）
  void harmonics(const InstrumentProfile *prof, float f0Play, float loud,
                 float tNorm, bool released,
                 float *ampOut, float *noiseOut, int nPartials) const;

  // 諧波實際頻率（含非諧性），h 從 0 起算
  float harmonicHz(const InstrumentProfile *prof, float f0Play, int h) const;

private:
  const InstrumentProfile *_p = nullptr;
  const ProfileBank       *_bank = nullptr;
  // 權重直接內含（7 KB）。早期版本是 file-scope static，結果多個 TimbreModel
  // 實例會共用同一份權重 —— 韌體只有一個實例所以沒事，但模擬器要同時比較
  // 兩個模型時會靜默地拿到同一份。改成成員變數就沒這問題。
  MlpWeights  _w;
  bool        _hasMlp = false;
  float       _blend  = TC_MLP_BLEND;

  void  runMlp(const float *in, float *out) const;
  void  keyframeLookup(const InstrumentProfile *prof, float tNorm,
                       bool released, float *out) const;
};

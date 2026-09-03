// ============================================================================
//  profile.h  -  一個「樂器音色指紋」的資料結構
//  分析器產生它、模型消費它、也可以存成 SD 上的 PROFILE.BIN
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"

struct InstrumentProfile {
  uint32_t magic;            // TC_PROFILE_MAGIC

  float f0;                  // 分析到的參考基頻 (Hz)
  float noteDur;             // 有效音長 (秒，從 onset 到尾巴)

  // --- 振幅包絡 (ADSR 擬合值，播放時用) ---
  float attack;              // 秒
  float decay;               // 秒
  float sustain;             // 0..1
  float release;             // 秒
  // 持續段每秒的自然衰減倍率：1.0 = 完全不掉(管風琴/弦樂長音)，
  // 0.5 = 每秒掉一半(鋼琴/撥弦)。只在 sustain 視窗內量測，不含 release。
  float sustainDecayPerSec;

  // --- 音色 ---
  // keyframe[k][h] = 第 k 個時間點、第 h 個諧波的相對振幅 (已正規化，總和=1)
  float keyframe[TC_N_KEYFRAME][TC_N_HARM];
  // loud[k] = 第 k 個時間點的整體響度 (0..1)
  //
  // ★ 這條曲線就是合成時實際使用的振幅包絡，不再走參數化 ADSR。
  //   原因：鋼琴是「雙段衰減」(prompt sound + aftersound)，實測前 0.3 秒
  //   衰減 0.026/秒、0.8 秒後只剩 0.70/秒，差了 27 倍。用單一指數擬合會得到
  //   0.475/秒 —— 對卡農那種 0.45~1.8 秒的音符來說慢了將近 20 倍，
  //   每個音都拖成管風琴一樣的長音。直接播量測到的曲線就沒有這個問題，
  //   而且提琴的漸強、管樂的起音全都自動正確。
  float loud[TC_N_KEYFRAME];

  // 包絡該在哪個（扭曲時間軸上的）位置停住不再往下走。
  //   衰減型 -> 1.0，整條曲線都要走完，衰減本身就是樂器的聲音
  //   持續型 -> 本體結束的位置，之後保持不變，不能把「收弓」演成自動消音
  float envHoldNorm;

  // --- 頻譜包絡 (共振峰)，log 頻率 50Hz..16kHz 均分 TC_SPECENV_PTS 點，單位 dB ---
  // 移調時用它做 formant 保留：換音高不換音色。
  float specEnv[TC_SPECENV_PTS];

  float noiseGain;           // 持續段的非諧波(氣聲/弓噪)能量比例 0..1
  float inharmonicity;       // B 係數，弦樂器 > 0，用來微調諧波頻率
  float brightness;          // 頻譜質心 / f0，除錯用

  // --- 真實度三要素（都是從素材量測出來的，不是猜的）---

  // 非同步起音：每根諧波抵達自身 50% 峰值的時刻，相對於基頻的延遲（秒）。
  // 真實樂器的高次諧波通常晚幾十毫秒才進來，全部同時起音會非常「電子」。
  float harmOnset[TC_N_HARM];

  // 逐根諧波在持續段的微觀起伏深度（標準差/平均，去除整體衰減後）。
  // 真實樂器約 5~10%，完全沒有的話長音聽起來像管風琴。
  float shimmerDepth;

  // 起音前 30 ms 的噪聲比例。弓噪／氣聲／擊弦雜音集中在這裡，
  // 通常遠高於持續段的 noiseGain。
  float attackNoise;

  // 從素材量到的顫音深度（cents）。提琴/人聲/管樂會有，鋼琴/吉他幾乎是 0。
  // 早期版本對所有樂器一律加 6 cents 的顫音 —— 鋼琴帶顫音物理上不可能，
  // 而且那個統一的擺動會讓每一種樂器都染上同樣的「合成器味」。
  float vibratoCents;
  float vibratoHz;           // 量測到的顫音頻率（無顫音時為 0）
  float noiseHighFrac;       // 持續段殘差在 5*f0 以上的比例（氣聲 vs 弓噪的落點）
  float attackHighFrac;      // 起音殘差在 5*f0 以上的比例（槌擊聲 vs 吹氣聲的落點）

  bool  valid;
};

// ============================================================================
//  ProfileBank  -  一把樂器的多個取樣點
//
//  只留一組 profile 的話，整個音域都得靠「移調 + 頻譜包絡校正」硬撐。
//  真實鋼琴每個音區的音色差很多（低音弦多、高音只有一根、擊弦點比例也不同），
//  移調兩個八度出來的東西一定不像。
//
//  有 12 個素材就存 12 組，每個音符挑最接近的那組來用，移調距離縮到半音，
//  誤差幾乎歸零。16 組約佔 75 KB DMAMEM。
// ============================================================================
#define TC_MAX_PROFILES 16

struct ProfileBank {
  InstrumentProfile p[TC_MAX_PROFILES];
  // 最近一次 add() 有沒有覺得「這好像不是同一把樂器」。
  // 面板要看得到 —— 採樣時使用者手上拿著樂器盯著 OLED，不會在看序列埠。
  bool  lastAddSuspect = false;
  float lastAddDist    = 0.0f;
  int n = 0;

  void clear() { n = 0; lastAddSuspect = false; lastAddDist = 0.0f; }
  bool add(const InstrumentProfile &np);        // 同音高會覆蓋
  void checkTimbreMismatch(const InstrumentProfile &np);   // add() 內部會呼叫
  int  nearest(float f0) const;                 // 找最接近的，空的回 -1

  // 庫滿了要犧牲誰。回傳該被換掉的索引，或 -1 代表「新的這組不值得換」。
  //
  // 拉成公開介面純粹是為了測試：這是純粹的取捨邏輯（只看音高，不碰音訊），
  // 桌機驗得完，而它一旦錯了是無聲的 —— 只會讓某個音區沒有素材可用。
  int  evictionTarget(float newF0) const;
  const InstrumentProfile *get(float f0) const;
  void summary() const;
};

bool  bankSave(const ProfileBank &b, const char *path);
bool  bankLoad(ProfileBank &b, const char *path);

bool  profileSave(const InstrumentProfile &p, const char *path);
bool  profileLoad(InstrumentProfile &p, const char *path);
void  profilePrint(const InstrumentProfile &p);

// 在 log 頻率軸上內插頻譜包絡，回傳線性增益 (非 dB)
float specEnvGain(const InstrumentProfile &p, float hz);

// 兩個音色的頻譜包絡差多少（dB，RMS）。
//
// 頻譜包絡刻意設計成與音高無關（它描述的是共振腔，不是被吹/彈的那個音），
// 所以同一把樂器的不同音高應該很接近，換一把樂器才會拉開。
// 用途：入庫時偵測「換了樂器卻忘了先清空」。
//
// 只比「形狀」不比絕對高度：兩邊各自扣掉自己的平均值再算差。
// 錄音音量不同不該被當成音色不同。
float profileEnvDistance(const InstrumentProfile &a, const InstrumentProfile &b);

// 綜合音色距離（無單位，1.0 ≈ 同樂器不同音高的典型差異）。
//
// 頻譜包絡單獨用不夠：實測鋼琴同樂器內部的包絡距離最大到 15 dB，比
// 「小號 vs 提琴」還遠 —— 因為鋼琴每個音的琴弦與擊槌都不一樣。
// 但鋼琴在「衰減速度」「非諧性」「shimmer」這三項上跟持續音樂器差得非常開，
// 合起來看才分得出「換了樂器」和「同一把樂器的不同音」。
float profileTimbreDistance(const InstrumentProfile &a, const InstrumentProfile &b);

// ============================================================================
//  additive_synth.h  -  自訂 AudioStream：TC_N_VOICES 複音的加法合成器
//
//  每個聲音 = 16 個正弦諧波 + 1 層濾波噪聲 + ADSR 包絡
//  諧波振幅每個 block (2.9 ms) 由 TimbreModel 更新一次，block 內線性斜坡，
//  所以聽起來是連續變化而不是階梯。
//
//  CPU 估算：16 諧波 × 128 取樣 × 6 複音 = 12288 次查表內插 / block
//            ≈ 0.12 M cycles / 2.9 ms，Teensy 4.1 (600 MHz) 約 7% CPU。
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Audio.h>
#include "config.h"
#include "timbre_model.h"

class AudioSynthAdditive : public AudioStream {
public:
  AudioSynthAdditive();

  void setModel(TimbreModel *m) { _model = m; }
  void setMasterGain(float g)   { _gain = tc_clampf(g, 0.0f, 1.0f); }
  // 顫音深度由 profile 決定（鋼琴量到 0 就不加）。這裡設的是「上限」與速率，
  // cents < 0 代表完全交給 profile。
  void setVibrato(float maxCents, float hz) { _vibMaxCents = maxCents; _vibHz = hz; }

  // 彎音輪：以半音為單位（Keystation 的 <PB / PB> 按鍵）。
  // 對已經在發聲的音也會即時生效，所以是全域倍率而不是 noteOn 時算死。
  void setPitchBend(float semitones) {
    _bendMul = powf(2.0f, tc_clampf(semitones, -24.0f, 24.0f) / 12.0f);
  }
  // 調變輪：在 profile 量到的顫音之上「額外」加的深度（cents）。
  // 用加的而不是取代，這樣鋼琴（量到 0）按 MOD 也會有反應。
  void setModDepth(float cents) { _modCents = tc_clampf(cents, 0.0f, 100.0f); }

  // noteOn 的結果。以前是 void，任何一種失敗都是「安靜地什麼都不做」——
  // 鍵盤按了沒聲音時完全查不出原因，所以改成回報。
  enum NoteResult : uint8_t {
    NOTE_OK = 0,
    NOTE_NO_MODEL,      // 還沒 setModel()
    NOTE_NO_TIMBRE,     // 沒有載入任何音色（SD 上沒有 BANK.BIN / PROFILE.BIN）
    NOTE_NO_VOICE       // 聲部全滿而且搶不到
  };
  NoteResult noteOn(float midi, float vel = 1.0f, float pan = 0.5f);

  bool hasTimbre() const { return _model && _model->ready(); }
  void noteOff(float midi);
  void allNotesOff();
  int  activeVoices() const;

  virtual void update(void);

private:
  // 包絡直接照著 profile 量到的曲線走，所以只需要兩個狀態。
  enum Stage : uint8_t { IDLE = 0, PLAYING, RELEASE };

  struct Voice {
    Stage    stage = IDLE;
    float    midi  = 0.0f;
    float    f0    = 0.0f;
    float    vel   = 0.0f;
    float    pan   = 0.5f;
    float    env   = 0.0f;
    float    tSec  = 0.0f;
    uint32_t age   = 0;

    int      nPart = TC_N_HARM;          // 這個音高實際要發幾根（依 f0 自動決定）
    uint32_t phase[TC_N_PARTIAL];
    uint32_t baseInc[TC_N_PARTIAL];
    float    amp[TC_N_PARTIAL];
    float    ampStep[TC_N_PARTIAL];

    // 非同步起音：每根諧波自己的進場時刻（秒）與閘門狀態
    float    onsetT[TC_N_PARTIAL];
    // shimmer：每根諧波獨立的慢速擺動
    float    shimPhase[TC_N_PARTIAL];
    float    shimInc[TC_N_PARTIAL];
    float    shimDepth = 0.0f;

    float    noise = 0.0f, noiseStep = 0.0f;
    // 噪聲帶通：TC_NOISE_LP_STAGES 級 biquad 低通 + 一級 biquad 高通。
    // 不用一階遞迴串接 —— 轉角一高它就退化成 y = x，等於沒有低通。
    float    nbLpB[3] = {1.0f, 0.0f, 0.0f}, nbLpA[2] = {0.0f, 0.0f};
    float    nbHpB[3] = {1.0f, 0.0f, 0.0f}, nbHpA[2] = {0.0f, 0.0f};
    float    nbLpZ[TC_NOISE_LP_STAGES][2] = {{0.0f, 0.0f}};
    float    nbHpZ[2] = {0.0f, 0.0f};
    float    noiseNrm = 1.0f;      // noteOn 時實測出來的濾波器 RMS 補償
    float    noiseFLo = 0.0f, noiseFHi = 0.0f;   // 實際用的帶通上下緣（除錯用）
    float    jitterSigma = 0.0f;   // 諧波幅度抖動的標準差
    float    jitFrac = 0.9f;       // 非週期能量中走「抖動」的比例
    float    atkJitVar = 0.0f, atkJitSigma = 0.0f;   // 起音期間額外的抖動
    float    jit[TC_N_PARTIAL] = {0};
    float    noiseAtk = 0.0f;            // 起音噪聲爆點的額外增益
    uint32_t rng   = 0x13579BDFu;

    float    vibPhase = 0.0f;
    float    vibCents = 0.0f;
    float    vibHz    = 4.8f;      // 顫音速率（來自 profile）

    // 由 profile 帶進來的包絡參數
    float    rCoef  = 0.999f;      // 放開後的衰減（每 block）
    float    refDur = 1.0f;        // 素材的音長，用來換算扭曲時間軸
    float    holdNorm = 1.0f;      // 包絡走到這裡就不再往下
    // 量到的包絡曲線走完之後，每個 block 還要再乘上多少。
    //
    // 衰減型（鋼琴/吉他/撥弦）取自 profile 的 sustainDecayPerSec；
    // 持續型是 1.0，也就是維持不變（運弓/吹氣還在繼續）。
    //
    // 沒有這個東西的話，音會停在 loud[31] 的高度不再往下 —— 實測鋼琴的
    // loud[] 只掉到 -24 dB 就結束，於是每個音都在 -24 dB 平住，
    // 聽起來像管風琴。素材錄得越短越明顯（音長 1.79 s 的錄音，
    // 卡農的二分音符還沒放開就已經走完包絡了）。
    float    tailCoef = 1.0f;
    float    envTail  = 0.0f;      // 曲線走完之後的包絡值；0 = 還沒進入尾段
    const InstrumentProfile *prof = nullptr;
  };

  Voice        _v[TC_N_VOICES];
  TimbreModel *_model = nullptr;
  float        _gain  = 0.18f;
  float        _vibMaxCents = 50.0f;
  float        _vibHz    = 4.8f;
  float        _bendMul  = 1.0f;
  float        _modCents = 0.0f;
  uint32_t     _ageCounter = 1;

  int  allocVoice(float midi);
  void renderVoice(Voice &v, float *dst);
};

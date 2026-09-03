#include "timbre_model.h"
#include <SD.h>

// ---------------------------------------------------------------------------
bool TimbreModel::loadWeights(const char *path) {
  _hasMlp = false;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.printf("[MLP] 找不到 %s，改用關鍵影格內插模式\n", path);
    return false;
  }
  if (f.size() != sizeof(MlpWeights)) {
    Serial.printf("[MLP] %s 大小不符 (%lu != %u)，忽略\n",
                  path, (unsigned long)f.size(), (unsigned)sizeof(MlpWeights));
    f.close();
    return false;
  }
  f.read((uint8_t *)&_w, sizeof(MlpWeights));
  f.close();

  if (_w.magic != TC_MLP_MAGIC) {
    Serial.println(F("[MLP] magic 不符，忽略"));
    return false;
  }
  _hasMlp = true;
  Serial.printf("[MLP] 已載入 %s  (%u bytes, %d-%d-%d-%d)\n",
                path, (unsigned)sizeof(MlpWeights),
                TC_MLP_IN, TC_MLP_H1, TC_MLP_H2, TC_MLP_OUT);
  return true;
}

// ---------------------------------------------------------------------------
void TimbreModel::adoptWeights(const MlpWeights &w) {
  if (w.magic != TC_MLP_MAGIC) { Serial.println(F("[MLP] magic 不符，不套用")); return; }
  memcpy(&_w, &w, sizeof(MlpWeights));
  _hasMlp = true;
  Serial.println(F("[MLP] 已套用新權重"));
}

bool TimbreModel::saveWeights(const MlpWeights &w, const char *path) const {
  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) { Serial.printf("[MLP] 無法建立 %s\n", path); return false; }
  f.write((const uint8_t *)&w, sizeof(MlpWeights));
  f.close();
  Serial.printf("[MLP] 已存檔 %s (%u bytes)\n", path, (unsigned)sizeof(MlpWeights));
  return true;
}

// tc_tanh / tc_sigmoid 定義在 config.h，訓練器用的是同一份 —— 這很重要，
// 否則訓練時用 tanhf、推論時用近似值，會產生 train/inference mismatch。
#define fastTanh    tc_tanh
#define fastSigmoid tc_sigmoid

// ---------------------------------------------------------------------------
void TimbreModel::runMlp(const float *in, float *out) const {
  float h1[TC_MLP_H1];
  float h2[TC_MLP_H2];

  for (int i = 0; i < TC_MLP_H1; i++) {
    float s = _w.b1[i];
    for (int j = 0; j < TC_MLP_IN; j++) s += _w.w1[i][j] * in[j];
    h1[i] = fastTanh(s);
  }
  for (int i = 0; i < TC_MLP_H2; i++) {
    float s = _w.b2[i];
    for (int j = 0; j < TC_MLP_H1; j++) s += _w.w2[i][j] * h1[j];
    h2[i] = fastTanh(s);
  }
  for (int i = 0; i < TC_MLP_OUT; i++) {
    float s = _w.b3[i];
    for (int j = 0; j < TC_MLP_H2; j++) s += _w.w3[i][j] * h2[j];
    out[i] = s;
  }

  // 前 TC_N_HARM 個做 softmax -> 諧波分佈；最後一個做 sigmoid -> 噪聲
  float mx = out[0];
  for (int i = 1; i < TC_N_HARM; i++) if (out[i] > mx) mx = out[i];
  float sum = 0.0f;
  for (int i = 0; i < TC_N_HARM; i++) { out[i] = expf(out[i] - mx); sum += out[i]; }
  float inv = 1.0f / (sum + 1e-9f);
  for (int i = 0; i < TC_N_HARM; i++) out[i] *= inv;
  out[TC_N_HARM] = fastSigmoid(out[TC_N_HARM]);
}

// ---------------------------------------------------------------------------
void TimbreModel::keyframeLookup(const InstrumentProfile *prof, float tNorm,
                                 bool released, float *out) const {
  const InstrumentProfile *_pp = prof;
  float pos = tc_clampf(tNorm, 0.0f, 0.999f) * (TC_N_KEYFRAME - 1);
  int   k   = (int)pos;
  if (k > TC_N_KEYFRAME - 2) k = TC_N_KEYFRAME - 2;
  float t   = pos - k;

  for (int h = 0; h < TC_N_HARM; h++)
    out[h] = _pp->keyframe[k][h] * (1.0f - t) + _pp->keyframe[k + 1][h] * t;

  // 放開後高次諧波衰減得比基頻快（真實樂器的共通行為）
  if (released) {
    float s = 1.0f;
    for (int h = 0; h < TC_N_HARM; h++) { out[h] *= s; s *= 0.93f; }
  }
  out[TC_N_HARM] = _pp->noiseGain;
}

// ---------------------------------------------------------------------------
float TimbreModel::harmonicHz(const InstrumentProfile *prof, float f0Play, int h) const {
  const float n = (float)(h + 1);
  if (!prof || prof->inharmonicity <= 0.0f) return f0Play * n;
  return f0Play * n * sqrtf(1.0f + prof->inharmonicity * n * n);   // 弦的非諧性
}

// ---------------------------------------------------------------------------
void TimbreModel::harmonics(const InstrumentProfile *prof, float f0Play, float loud,
                            float tNorm, bool released,
                            float *ampOut, float *noiseOut, int nPartials) const {
  if (!prof) prof = _p;
  if (nPartials < 1) nPartials = 1;
  if (nPartials > TC_N_PARTIAL) nPartials = TC_N_PARTIAL;

  if (!prof || !prof->valid) {
    for (int h = 0; h < nPartials; h++) ampOut[h] = 0.0f;
    ampOut[0] = loud;
    *noiseOut = 0.0f;
    return;
  }

  float raw[TC_MLP_OUT];

  // 關鍵影格是「直接量到的」分佈，最忠實；MLP 是學出來的，會有平滑與洩漏。
  // 實測用真實鋼琴素材：關鍵影格 51.3/35.0/9.4/1.3/1.4/0.7/0.9（真值
  // 53.8/34.5/8.7/1.5/1.4/0.0/0.0）幾乎完全吻合，MLP 卻把 h2 壓低、
  // 在 h4~h7 生出真實樂器沒有的能量。
  //
  // 所以改成「以關鍵影格為基底、MLP 只做修正」的對數域混合：
  //   BLEND = 0 完全信關鍵影格，1 完全信 MLP。
  // 這樣 MLP 保留了它真正的價值（隨響度/音高微調），卻不可能破壞基本音色。
  keyframeLookup(prof, tNorm, released, raw);

  // 守衛條件是 mlpActive() 而不是 _hasMlp：blend 為 0 時整段推論的結果會被
  // 混合式子完全抵銷（exp(lk + 0*(lm-lk)) == exp(lk)），跑了等於白跑。
  // 實測代價不小 —— 每個聲部每個 block 一次 2208 MAC，外加 32 對 logf/expf
  // （M7 上各數十 cycle），6 複音約 3% CPU 花在一個沒有效果的運算上。
  if (mlpActive()) {
    float mlpOut[TC_MLP_OUT];
    float in[TC_MLP_IN];
    in[0] = tc_clampf(log2f(f0Play / 261.63f) / TC_MLP_PITCH_SCALE, -3.0f, 3.0f);
    in[1] = tc_clampf(loud, 0.0f, 1.0f);
    in[2] = tc_clampf(tNorm, 0.0f, 1.5f);
    in[3] = released ? 1.0f : 0.0f;
    runMlp(in, mlpOut);

    const float B = _blend;
    float sum = 0.0f;
    for (int h = 0; h < TC_N_HARM; h++) {
      float lk = logf(raw[h]    + 1e-6f);
      float lm = logf(mlpOut[h] + 1e-6f);
      raw[h] = expf(lk + B * (lm - lk));
      sum += raw[h];
    }
    if (sum > 1e-9f) { float k = 1.0f / sum; for (int h = 0; h < TC_N_HARM; h++) raw[h] *= k; }
    raw[TC_N_HARM] += B * (mlpOut[TC_N_HARM] - raw[TC_N_HARM]);
  }

  // ---- 頻譜包絡(共振峰)校正 + 抗混疊 -------------------------------------
  // 原始 keyframe/MLP 給的是「參考基頻 _p->f0 下」的諧波分佈。
  // 換到 f0Play 時，讓每個諧波去查它「新的絕對頻率」在包絡上的增益，
  // 再除掉它在參考音高下的增益 —— 這樣共振峰留在原位，音色不會變聲。
  const float nyq = TC_SAMPLE_RATE * TC_NYQUIST_GUARD;
  float energy = 0.0f;

  // 前 TC_N_HARM 根：模型直接給
  int nModel = (nPartials < TC_N_HARM) ? nPartials : TC_N_HARM;
  for (int h = 0; h < nModel; h++) {
    float fNew = harmonicHz(prof, f0Play, h);
    if (fNew >= nyq) { ampOut[h] = 0.0f; continue; }

    // 靠近 Nyquist 的諧波做 cosine 淡出，避免混疊刺耳
    float roll = 1.0f;
    if (fNew > nyq * 0.8f) roll = 0.5f * (1.0f + cosf((float)M_PI * (fNew - nyq * 0.8f) / (nyq * 0.2f)));

#if TC_TRANSPOSE_RESAMPLE
    // 目標的第 h 根落在來源的第幾根（連續值，0 起算）。
    // 直接問「來源在這個絕對頻率上有多大聲」—— 共振峰自然留在原位，
    // 而且拿到的是**那個頻率**的時間軌跡，不是第 h 根的。見 config.h。
    const float hSrcF = fNew / prof->f0 - 1.0f;
    float a;
    if (hSrcF <= (float)(TC_N_HARM - 1)) {
      const int   i0 = (hSrcF > 0.0f) ? (int)hSrcF : 0;
      const int   i1 = (i0 + 1 < TC_N_HARM) ? (i0 + 1) : (TC_N_HARM - 1);
      const float fr = tc_clampf(hSrcF - (float)i0, 0.0f, 1.0f);
      // 對數域內插：諧波分佈的動態範圍很大，線性內插會被大的那一根主導
      a = expf((1.0f - fr) * logf(raw[i0] + 1e-6f) + fr * logf(raw[i1] + 1e-6f));
    } else {
      // 已經超出量測到的 32 根，只能用包絡從最後一根往外推
      const float fLast = prof->f0 * (float)TC_N_HARM;
      const float ratio = specEnvGain(*prof, fNew) / (specEnvGain(*prof, fLast) + 1e-6f);
      a = raw[TC_N_HARM - 1] * tc_clampf(ratio, 0.0f, 1.5f);
    }
    // 跟舊作法做對數域混合，權重 TC_TRANSPOSE_RESAMPLE（1 = 全用重取樣）。
    // 留這個旋鈕是因為兩者各有弱點：重取樣拿到的是正確頻率上的時間軌跡，
    // 但非八度的移調會落在兩根諧波之間，內插到的可能是「谷底」而不是包絡；
    // 舊作法用平滑過的包絡沒有這個漣漪，卻搬錯了時間軌跡。
    {
      const float fRef = prof->f0 * (h + 1);
      float g = specEnvGain(*prof, fNew) / (specEnvGain(*prof, fRef) + 1e-6f);
      g = tc_clampf(g, 0.05f, 4.0f);        // 不讓包絡校正暴衝
      const float aOld = raw[h] * g;
      // 權重跟著移調距離走。
      //
      // 重取樣的代價是內插誤差：目標的諧波落在來源兩根之間時，取到的可能是
      // 「谷底」而不是包絡。這個誤差跟索引的小數部分成正比，而小數部分
      // 又跟音高比例的偏差成正比 —— 所以移調越少，代價越大而好處越小。
      //
      // 實測：音色庫涵蓋得到的音（演奏音高與 profile 只差幾個 cent，
      // 但第 30 根諧波的索引已經偏了 0.12）如果也走重取樣，吉他的
      // LSD 中段會從 0.59 掉到 0.85 —— 那是純粹的內插漣漪，沒有換到任何東西。
      // 一個半音以內完全不用，四個半音以上全用。
      const float semi = fabsf(12.0f * log2f(f0Play / (prof->f0 + 1e-6f)));
      const float w    = TC_TRANSPOSE_RESAMPLE_W
                       * tc_clampf((semi - TC_TRANSPOSE_RESAMPLE_LO)
                                   / (TC_TRANSPOSE_RESAMPLE_HI - TC_TRANSPOSE_RESAMPLE_LO),
                                   0.0f, 1.0f);
      ampOut[h] = expf((1.0f - w) * logf(aOld + 1e-9f) + w * logf(a + 1e-9f)) * roll;
    }
#else
    float fRef = prof->f0 * (h + 1);
    float g    = specEnvGain(*prof, fNew) / (specEnvGain(*prof, fRef) + 1e-6f);
    g = tc_clampf(g, 0.05f, 4.0f);          // 不讓包絡校正暴衝
    ampOut[h] = raw[h] * g * roll;
#endif
    energy   += ampOut[h] * ampOut[h];
  }

  // 第 33 根以上：改用「量測到的頻譜包絡」外推。
  // 真實錄音超過 32 根多半已埋進噪聲，逐根建模等於在學雜訊；用包絡穩健得多。
  // 低音全靠這段才不會悶：D2 舊版最高只到 1184 Hz，現在能到 4.7 kHz。
  if (nPartials > TC_N_HARM) {
    const float fAnchor = harmonicHz(prof, f0Play, TC_N_HARM - 1);
    const float gAnchor = specEnvGain(*prof, fAnchor) + 1e-9f;
    const float aAnchor = ampOut[TC_N_HARM - 1];

    for (int h = TC_N_HARM; h < nPartials; h++) {
      float fNew = harmonicHz(prof, f0Play, h);
      if (fNew >= nyq) { ampOut[h] = 0.0f; continue; }

      float ratio = specEnvGain(*prof, fNew) / gAnchor;
      float roll  = 1.0f;
      if (fNew > nyq * 0.8f)
        roll = 0.5f * (1.0f + cosf((float)M_PI * (fNew - nyq * 0.8f) / (nyq * 0.2f)));

      ampOut[h] = aAnchor * tc_clampf(ratio, 0.0f, 1.5f) * roll;
      energy   += ampOut[h] * ampOut[h];
    }
  }

  // 用「能量(L2)」而不是「振幅總和(L1)」正規化。
  // 這點很重要：鋼琴這類樂器的高次諧波衰減得比基頻快，頻譜會愈來愈集中，
  // 如果用 L1 正規化，能量反而會隨著集中而上升，聽起來就不會衰減。
  // 改用 L2 之後，輸出的 RMS 才會忠實跟著 ADSR 包絡走。
  // ---- 諧波與噪聲的能量分配 ----------------------------------------------
  //
  // raw[TC_N_HARM] 是分析器量到的「非週期成分佔總能量的比例」。
  //
  // 早期版本直接把它當成振幅倍率用 —— 那是單位錯誤。能量比 0.004 對應的
  // 振幅比是 sqrt(0.004) = 0.063，差了 16 倍（24 dB）。實測長笛的諧波間
  // 底噪：真實 -72.6 dB、合成 -99.5 dB，正好就是這個量級。
  // 對鋼琴/提琴影響不明顯（諧波遠大於噪聲），但長笛的氣聲就是它的特徵，
  // 少了那層底噪聽起來就變成管風琴。
  const float noiseFrac = tc_clampf(raw[TC_N_HARM], 0.0f, 0.9f);
  const float harmAmp   = sqrtf(1.0f - noiseFrac);
  // 只有「殘差落在 5*f0 以上」的那一份走寬頻噪聲層，其餘由 additive_synth
  // 的諧波抖動產生（能量分佈才會落在正確的頻率位置）。比例來自量測。
  const float noiseAmp  = sqrtf(noiseFrac * tc_clampf(prof->noiseHighFrac, 0.0f, 0.9f));

  float rms = sqrtf(energy);
  if (rms > 1e-9f) {
    // sqrt(2) 是因為正弦的 RMS 是振幅的 1/sqrt(2)：這樣輸出訊號的 RMS 恰好等於 loud
    float k = loud * 1.41421356f * harmAmp / rms;
    for (int h = 0; h < nPartials; h++) ampOut[h] *= k;
  }

  *noiseOut = noiseAmp * loud;
}

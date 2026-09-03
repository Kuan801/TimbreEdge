#include "trainer.h"

// ============================================================================
//  記憶體配置：全部丟 DMAMEM (OCRAM)，把 DTCM 留給 audio ISR
// ============================================================================
DMAMEM static TrainSample gSamples[TC_TRAIN_MAX];    // 215 KB
DMAMEM static float       gGrad[TC_MLP_NPARAM];      //  7 KB
DMAMEM static float       gAdamM[TC_MLP_NPARAM];     //  7 KB
DMAMEM static float       gAdamV[TC_MLP_NPARAM];     //  7 KB

// MlpWeights 必須是「magic + TC_MLP_NPARAM 個緊密排列的 float」，Adam 才能把它當
// 一維陣列來更新，而且存出去的 MODEL.BIN 才跟 Python 版二進位相容。
static_assert(sizeof(MlpWeights) == sizeof(uint32_t) + TC_MLP_NPARAM * sizeof(float),
              "MlpWeights 出現 padding，攤平索引會錯位");

// 攤平索引的起點
#define OFF_W1 0
#define OFF_B1 (OFF_W1 + TC_MLP_H1 * TC_MLP_IN)
#define OFF_W2 (OFF_B1 + TC_MLP_H1)
#define OFF_B2 (OFF_W2 + TC_MLP_H2 * TC_MLP_H1)
#define OFF_W3 (OFF_B2 + TC_MLP_H2)
#define OFF_B3 (OFF_W3 + TC_MLP_OUT * TC_MLP_H2)

static inline float *flatOf(MlpWeights &w) { return &w.w1[0][0]; }

static void (*gProgressCb)(int, int, float, float) = nullptr;
void trainerSetProgressCallback(void (*cb)(int, int, float, float)) { gProgressCb = cb; }

// ============================================================================
//  亂數：xorshift32 + Box-Muller
// ============================================================================
static uint32_t gRng = 2463534242u;
static inline uint32_t xrand() {
  gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5;
  return gRng;
}
static inline float urand() { return (float)(xrand() >> 8) * (1.0f / 16777216.0f); }
static float nrand() {
  float u1 = urand(); if (u1 < 1e-7f) u1 = 1e-7f;
  float u2 = urand();
  return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

// ============================================================================
//  TrainSet
// ============================================================================
void TrainSet::clear() { _n = 0; _warned = false; }

const TrainSample *TrainSet::data() const { return gSamples; }

bool TrainSet::add(const float *in, const float *harm, float noise) {
  if (_n >= TC_TRAIN_MAX) {
    if (!_warned) {
      Serial.printf("[TRAIN] 訓練集已滿 (%d 筆)，後續資料忽略。"
                    "可調大 config.h 的 TC_TRAIN_MAX\n", TC_TRAIN_MAX);
      _warned = true;
    }
    return false;
  }
  TrainSample &s = gSamples[_n++];
  for (int i = 0; i < TC_MLP_IN; i++)   s.in[i]   = in[i];
  for (int h = 0; h < TC_N_HARM; h++)   s.harm[h] = tc_quant(harm[h]);
  s.noise = tc_quant(noise);
  s._pad  = 0;
  return true;
}

int TrainSet::pitchCount() const {
  // in[0] = clip(log2(f0/261.63)/3)。同一個音的所有格 in[0] 完全相同，
  // 所以直接用 0.005 的容差分群就夠（約半個半音）。
  float seen[16];
  int   k = 0;
  for (int i = 0; i < _n; i++) {
    bool dup = false;
    for (int j = 0; j < k; j++)
      if (fabsf(gSamples[i].in[0] - seen[j]) < 0.005f) { dup = true; break; }
    if (!dup && k < 16) seen[k++] = gSamples[i].in[0];
  }
  return k;
}

void TrainSet::summary() const {
  if (_n == 0) { Serial.println(F("[TRAIN] 訓練集是空的。用「n 檔名.WAV」加素材。")); return; }
  int np = pitchCount();
  Serial.printf("[TRAIN] 訓練集：%d 筆樣本，涵蓋 %d 個音高，佔用 %lu KB\n",
                _n, np, (unsigned long)(_n * sizeof(TrainSample) / 1024));
  if (np < 2)
    Serial.println(F("        只有一個音高：MLP 只會學到「時間/響度」的影響。"
                     "跨音高的部分由頻譜包絡校正負責，但多幾個音高效果更好。"));
}

// ============================================================================
//  前向 / 反向
// ============================================================================
// tanh / sigmoid 一律用 config.h 裡的 tc_tanh / tc_sigmoid —— 跟推論路徑
// 完全同一份實作。這不只是為了快（M7 的 tanhf 要 50~100 cycle），更是為了
// 避免「訓練用精確 tanh、推論用近似值」造成的 train/inference mismatch。

// 前向，回傳各層啟動值。z3 為 logits。
static void forward(const MlpWeights &w, const float *x,
                    float *a1, float *a2, float *z3) {
  for (int i = 0; i < TC_MLP_H1; i++) {
    float s = w.b1[i];
    for (int j = 0; j < TC_MLP_IN; j++) s += w.w1[i][j] * x[j];
    a1[i] = tc_tanh(s);
  }
  for (int i = 0; i < TC_MLP_H2; i++) {
    float s = w.b2[i];
    for (int j = 0; j < TC_MLP_H1; j++) s += w.w2[i][j] * a1[j];
    a2[i] = tc_tanh(s);
  }
  for (int i = 0; i < TC_MLP_OUT; i++) {
    float s = w.b3[i];
    for (int j = 0; j < TC_MLP_H2; j++) s += w.w3[i][j] * a2[j];
    z3[i] = s;
  }
}

// logits -> 機率。前 TC_N_HARM (32) 個 softmax，最後一個 sigmoid。
static void activate(float *z3, float *ph, float *pn) {
  float mx = z3[0];
  for (int i = 1; i < TC_N_HARM; i++) if (z3[i] > mx) mx = z3[i];
  float sum = 0.0f;
  for (int i = 0; i < TC_N_HARM; i++) { ph[i] = expf(z3[i] - mx); sum += ph[i]; }
  float inv = 1.0f / (sum + 1e-9f);
  for (int i = 0; i < TC_N_HARM; i++) ph[i] *= inv;
  *pn = tc_sigmoid(z3[TC_N_HARM]);
}

// 全資料集評估：交叉熵 + 諧波平均絕對誤差
static void evaluate(const MlpWeights &w, const TrainSample *s, int n,
                     float *ceOut, float *maeOut) {
  float a1[TC_MLP_H1], a2[TC_MLP_H2], z3[TC_MLP_OUT], ph[TC_N_HARM], pn;
  double ce = 0.0, mae = 0.0;
  for (int i = 0; i < n; i++) {
    forward(w, s[i].in, a1, a2, z3);
    activate(z3, ph, &pn);
    for (int h = 0; h < TC_N_HARM; h++) {
      float y = tc_dequant(s[i].harm[h]);
      ce  -= (double)y * logf(ph[h] + 1e-9f);
      mae += fabsf(ph[h] - y);
    }
  }
  *ceOut  = (float)(ce / n);
  *maeOut = (float)(mae / ((double)n * TC_N_HARM));
}

// ============================================================================
bool trainMlp(const TrainSet &ts, MlpWeights &out, int epochs, float lr,
              uint32_t seed, int progressEvery) {
  const int n = ts.size();
  if (n < 32) {
    Serial.printf("[TRAIN] 樣本太少 (%d 筆，至少要 32)。先用「n 檔名.WAV」加素材。\n", n);
    return false;
  }
  const TrainSample *S = ts.data();
  const int batch = (n < TC_TRAIN_BATCH) ? n : TC_TRAIN_BATCH;

  gRng = seed ? seed : 1u;
  float *W = flatOf(out);

  // ---- Xavier / LeCun 初始化 ---------------------------------------------
  //
  // 標準差取 sqrt(1/fan_in)。這裡刻意不是 He 初始化（sqrt(2/fan_in)）——
  // He 是為 ReLU 設計的（要補償負半邊被歸零掉的變異數），
  // 而本網路隱藏層用 tanh，tanh 在原點附近的斜率接近 1，
  // 用 sqrt(1/fan_in) 才能讓各層的啟動值變異數維持穩定。
  //
  // 舊註解寫「He 初始化」是誤植，程式碼本身一直是對的。
  out.magic = TC_MLP_MAGIC;
  for (int i = 0; i < TC_MLP_NPARAM; i++) W[i] = 0.0f;
  {
    float s1 = sqrtf(1.0f / TC_MLP_IN);
    float s2 = sqrtf(1.0f / TC_MLP_H1);
    float s3 = sqrtf(1.0f / TC_MLP_H2);
    for (int i = 0; i < TC_MLP_H1 * TC_MLP_IN; i++)  W[OFF_W1 + i] = nrand() * s1;
    for (int i = 0; i < TC_MLP_H2 * TC_MLP_H1; i++)  W[OFF_W2 + i] = nrand() * s2;
    for (int i = 0; i < TC_MLP_OUT * TC_MLP_H2; i++) W[OFF_W3 + i] = nrand() * s3;
    // bias 全部留 0
  }
  for (int i = 0; i < TC_MLP_NPARAM; i++) { gAdamM[i] = 0.0f; gAdamV[i] = 0.0f; }

  const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
  float b1t = 1.0f, b2t = 1.0f;
  const float invB = 1.0f / (float)batch;

  uint32_t t0 = millis();
  Serial.printf("[TRAIN] 開始：%d 筆樣本，batch %d，%d epochs，lr %.4f\n",
                n, batch, epochs, lr);
  {
    float ce, mae;
    evaluate(out, S, n, &ce, &mae);
    Serial.printf("        epoch %5d   CE %.4f   平均諧波誤差 %.5f\n", 0, ce, mae);
  }

  // ---- 主迴圈 -------------------------------------------------------------
  float a1[TC_MLP_H1], a2[TC_MLP_H2], z3[TC_MLP_OUT];
  float ph[TC_N_HARM], pn;
  float dz3[TC_MLP_OUT], dz2[TC_MLP_H2], dz1[TC_MLP_H1];

  for (int ep = 1; ep <= epochs; ep++) {
    for (int i = 0; i < TC_MLP_NPARAM; i++) gGrad[i] = 0.0f;

    for (int b = 0; b < batch; b++) {
      const TrainSample &s = S[xrand() % (uint32_t)n];

      forward(out, s.in, a1, a2, z3);
      activate(z3, ph, &pn);

      // softmax+CE 與 sigmoid+BCE 對 logits 的梯度都是 (預測 - 目標)
      for (int i = 0; i < TC_N_HARM; i++) dz3[i] = (ph[i] - tc_dequant(s.harm[i])) * invB;
      dz3[TC_N_HARM] = TC_TRAIN_NOISE_W * (pn - tc_dequant(s.noise)) * invB;

      // 第 3 層
      for (int i = 0; i < TC_MLP_OUT; i++) {
        float d = dz3[i];
        if (d == 0.0f) continue;
        float *g = &gGrad[OFF_W3 + i * TC_MLP_H2];
        for (int j = 0; j < TC_MLP_H2; j++) g[j] += d * a2[j];
        gGrad[OFF_B3 + i] += d;
      }
      // 回傳到第 2 層
      for (int j = 0; j < TC_MLP_H2; j++) {
        float s2 = 0.0f;
        for (int i = 0; i < TC_MLP_OUT; i++) s2 += dz3[i] * out.w3[i][j];
        dz2[j] = s2 * (1.0f - a2[j] * a2[j]);
      }
      for (int i = 0; i < TC_MLP_H2; i++) {
        float d = dz2[i];
        float *g = &gGrad[OFF_W2 + i * TC_MLP_H1];
        for (int j = 0; j < TC_MLP_H1; j++) g[j] += d * a1[j];
        gGrad[OFF_B2 + i] += d;
      }
      // 回傳到第 1 層
      for (int j = 0; j < TC_MLP_H1; j++) {
        float s1 = 0.0f;
        for (int i = 0; i < TC_MLP_H2; i++) s1 += dz2[i] * out.w2[i][j];
        dz1[j] = s1 * (1.0f - a1[j] * a1[j]);
      }
      for (int i = 0; i < TC_MLP_H1; i++) {
        float d = dz1[i];
        float *g = &gGrad[OFF_W1 + i * TC_MLP_IN];
        for (int j = 0; j < TC_MLP_IN; j++) g[j] += d * s.in[j];
        gGrad[OFF_B1 + i] += d;
      }
    }

    // ---- Adam ------------------------------------------------------------
    b1t *= b1;
    b2t *= b2;
    const float c1 = 1.0f / (1.0f - b1t);
    const float c2 = 1.0f / (1.0f - b2t);
    for (int i = 0; i < TC_MLP_NPARAM; i++) {
      float g = gGrad[i];
      gAdamM[i] = b1 * gAdamM[i] + (1.0f - b1) * g;
      gAdamV[i] = b2 * gAdamV[i] + (1.0f - b2) * g * g;
      W[i] -= lr * (gAdamM[i] * c1) / (sqrtf(gAdamV[i] * c2) + eps);
    }

    if (progressEvery > 0 && (ep % progressEvery == 0)) {
      float ce, mae;
      evaluate(out, S, n, &ce, &mae);
      Serial.printf("        epoch %5d   CE %.4f   平均諧波誤差 %.5f   (%lu s)\n",
                    ep, ce, mae, (unsigned long)((millis() - t0) / 1000));
      if (gProgressCb) gProgressCb(ep, epochs, ce, mae);
      if (!(ce == ce)) {                        // NaN 檢查
        Serial.println(F("[TRAIN] 發散了 (NaN)。把 lr 調小再試一次。"));
        return false;
      }
    }
  }

  float ce, mae;
  evaluate(out, S, n, &ce, &mae);
  Serial.printf("[TRAIN] 完成，耗時 %.1f 秒。最終 CE %.4f，平均諧波誤差 %.5f\n",
                (millis() - t0) / 1000.0f, ce, mae);
  if (mae < 0.002f)      Serial.println(F("        品質：很好"));
  else if (mae < 0.01f)  Serial.println(F("        品質：可用"));
  else                   Serial.println(F("        品質：偏差偏大，檢查素材是不是抓錯基頻或有雜訊"));

  out.magic = TC_MLP_MAGIC;
  return true;
}

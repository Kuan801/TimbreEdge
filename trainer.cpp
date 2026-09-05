#include "trainer.h"

// ============================================================================
//  Memory layout: everything goes in DMAMEM (OCRAM), leaving DTCM for the audio ISR
// ============================================================================
DMAMEM static TrainSample gSamples[TC_TRAIN_MAX];    // 215 KB
DMAMEM static float       gGrad[TC_MLP_NPARAM];      //  7 KB
DMAMEM static float       gAdamM[TC_MLP_NPARAM];     //  7 KB
DMAMEM static float       gAdamV[TC_MLP_NPARAM];     //  7 KB

// MlpWeights has to be "magic + TC_MLP_NPARAM tightly packed floats", so that Adam can update
// it as a flat array and the MODEL.BIN written out stays binary-compatible with the Python version.
static_assert(sizeof(MlpWeights) == sizeof(uint32_t) + TC_MLP_NPARAM * sizeof(float),
              "MlpWeights 出現 padding，攤平索引會錯位");

// Start of the flattened index
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
//  Random numbers: xorshift32 + Box-Muller
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
  // in[0] = clip(log2(f0/261.63)/3). Every frame of the same note has an identical in[0],
  // so grouping with a tolerance of 0.005 (about half a semitone) is enough.
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
//  Forward / backward
// ============================================================================
// tanh / sigmoid always come from tc_tanh / tc_sigmoid in config.h -- literally the same
// implementation as the inference path. Not just for speed (tanhf costs 50~100 cycles on the
// M7), but to avoid the train/inference mismatch of "exact tanh in training, an approximation at inference".

// Forward pass, returning the activations of each layer. z3 are the logits.
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

// logits -> probabilities. softmax over the first TC_N_HARM (32), sigmoid on the last one.
static void activate(float *z3, float *ph, float *pn) {
  float mx = z3[0];
  for (int i = 1; i < TC_N_HARM; i++) if (z3[i] > mx) mx = z3[i];
  float sum = 0.0f;
  for (int i = 0; i < TC_N_HARM; i++) { ph[i] = expf(z3[i] - mx); sum += ph[i]; }
  float inv = 1.0f / (sum + 1e-9f);
  for (int i = 0; i < TC_N_HARM; i++) ph[i] *= inv;
  *pn = tc_sigmoid(z3[TC_N_HARM]);
}

// Whole-dataset evaluation: cross-entropy + mean absolute error of the partials
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

  // ---- Xavier / LeCun initialization -------------------------------------
  //
  // The standard deviation is sqrt(1/fan_in). Deliberately not He init (sqrt(2/fan_in)) --
  // He is designed for ReLU (it compensates for the variance killed off on the negative half),
  // whereas the hidden layers here use tanh, whose slope near the origin is close to 1,
  // and only sqrt(1/fan_in) keeps the activation variance stable from layer to layer.
  //
  // The old comment saying "He initialization" was a slip; the code itself was always right.
  out.magic = TC_MLP_MAGIC;
  for (int i = 0; i < TC_MLP_NPARAM; i++) W[i] = 0.0f;
  {
    float s1 = sqrtf(1.0f / TC_MLP_IN);
    float s2 = sqrtf(1.0f / TC_MLP_H1);
    float s3 = sqrtf(1.0f / TC_MLP_H2);
    for (int i = 0; i < TC_MLP_H1 * TC_MLP_IN; i++)  W[OFF_W1 + i] = nrand() * s1;
    for (int i = 0; i < TC_MLP_H2 * TC_MLP_H1; i++)  W[OFF_W2 + i] = nrand() * s2;
    for (int i = 0; i < TC_MLP_OUT * TC_MLP_H2; i++) W[OFF_W3 + i] = nrand() * s3;
    // All biases left at 0
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

  // ---- Main loop ----------------------------------------------------------
  float a1[TC_MLP_H1], a2[TC_MLP_H2], z3[TC_MLP_OUT];
  float ph[TC_N_HARM], pn;
  float dz3[TC_MLP_OUT], dz2[TC_MLP_H2], dz1[TC_MLP_H1];

  for (int ep = 1; ep <= epochs; ep++) {
    for (int i = 0; i < TC_MLP_NPARAM; i++) gGrad[i] = 0.0f;

    for (int b = 0; b < batch; b++) {
      const TrainSample &s = S[xrand() % (uint32_t)n];

      forward(out, s.in, a1, a2, z3);
      activate(z3, ph, &pn);

      // For both softmax+CE and sigmoid+BCE the gradient w.r.t. the logits is (prediction - target)
      for (int i = 0; i < TC_N_HARM; i++) dz3[i] = (ph[i] - tc_dequant(s.harm[i])) * invB;
      dz3[TC_N_HARM] = TC_TRAIN_NOISE_W * (pn - tc_dequant(s.noise)) * invB;

      // Layer 3
      for (int i = 0; i < TC_MLP_OUT; i++) {
        float d = dz3[i];
        if (d == 0.0f) continue;
        float *g = &gGrad[OFF_W3 + i * TC_MLP_H2];
        for (int j = 0; j < TC_MLP_H2; j++) g[j] += d * a2[j];
        gGrad[OFF_B3 + i] += d;
      }
      // Back to layer 2
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
      // Back to layer 1
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
      if (!(ce == ce)) {                        // NaN check
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

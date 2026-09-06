#include "timbre_model.h"
#include <SD.h>

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

#define fastTanh    tc_tanh
#define fastSigmoid tc_sigmoid

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

  float mx = out[0];
  for (int i = 1; i < TC_N_HARM; i++) if (out[i] > mx) mx = out[i];
  float sum = 0.0f;
  for (int i = 0; i < TC_N_HARM; i++) { out[i] = expf(out[i] - mx); sum += out[i]; }
  float inv = 1.0f / (sum + 1e-9f);
  for (int i = 0; i < TC_N_HARM; i++) out[i] *= inv;
  out[TC_N_HARM] = fastSigmoid(out[TC_N_HARM]);
}

void TimbreModel::keyframeLookup(const InstrumentProfile *prof, float tNorm,
                                 bool released, float *out) const {
  const InstrumentProfile *_pp = prof;
  float pos = tc_clampf(tNorm, 0.0f, 0.999f) * (TC_N_KEYFRAME - 1);
  int   k   = (int)pos;
  if (k > TC_N_KEYFRAME - 2) k = TC_N_KEYFRAME - 2;
  float t   = pos - k;

  for (int h = 0; h < TC_N_HARM; h++)
    out[h] = _pp->keyframe[k][h] * (1.0f - t) + _pp->keyframe[k + 1][h] * t;

  if (released) {
    float s = 1.0f;
    for (int h = 0; h < TC_N_HARM; h++) { out[h] *= s; s *= 0.93f; }
  }
  out[TC_N_HARM] = _pp->noiseGain;
}

float TimbreModel::harmonicHz(const InstrumentProfile *prof, float f0Play, int h) const {
  const float n = (float)(h + 1);
  if (!prof || prof->inharmonicity <= 0.0f) return f0Play * n;
  return f0Play * n * sqrtf(1.0f + prof->inharmonicity * n * n);
}

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

  keyframeLookup(prof, tNorm, released, raw);

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

  const float nyq = TC_SAMPLE_RATE * TC_NYQUIST_GUARD;
  float energy = 0.0f;

  int nModel = (nPartials < TC_N_HARM) ? nPartials : TC_N_HARM;
  for (int h = 0; h < nModel; h++) {
    float fNew = harmonicHz(prof, f0Play, h);
    if (fNew >= nyq) { ampOut[h] = 0.0f; continue; }

    float roll = 1.0f;
    if (fNew > nyq * 0.8f) roll = 0.5f * (1.0f + cosf((float)M_PI * (fNew - nyq * 0.8f) / (nyq * 0.2f)));

#if TC_TRANSPOSE_RESAMPLE

    const float hSrcF = fNew / prof->f0 - 1.0f;
    float a;
    if (hSrcF <= (float)(TC_N_HARM - 1)) {
      const int   i0 = (hSrcF > 0.0f) ? (int)hSrcF : 0;
      const int   i1 = (i0 + 1 < TC_N_HARM) ? (i0 + 1) : (TC_N_HARM - 1);
      const float fr = tc_clampf(hSrcF - (float)i0, 0.0f, 1.0f);

      a = expf((1.0f - fr) * logf(raw[i0] + 1e-6f) + fr * logf(raw[i1] + 1e-6f));
    } else {

      const float fLast = prof->f0 * (float)TC_N_HARM;
      const float ratio = specEnvGain(*prof, fNew) / (specEnvGain(*prof, fLast) + 1e-6f);
      a = raw[TC_N_HARM - 1] * tc_clampf(ratio, 0.0f, 1.5f);
    }

    {
      const float fRef = prof->f0 * (h + 1);
      float g = specEnvGain(*prof, fNew) / (specEnvGain(*prof, fRef) + 1e-6f);
      g = tc_clampf(g, 0.05f, 4.0f);
      const float aOld = raw[h] * g;

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
    g = tc_clampf(g, 0.05f, 4.0f);
    ampOut[h] = raw[h] * g * roll;
#endif
    energy   += ampOut[h] * ampOut[h];
  }

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

  const float noiseFrac = tc_clampf(raw[TC_N_HARM], 0.0f, 0.9f);
  const float harmAmp   = sqrtf(1.0f - noiseFrac);

  const float noiseAmp  = sqrtf(noiseFrac * tc_clampf(prof->noiseHighFrac, 0.0f, 0.9f));

  float rms = sqrtf(energy);
  if (rms > 1e-9f) {

    float k = loud * 1.41421356f * harmAmp / rms;
    for (int h = 0; h < nPartials; h++) ampOut[h] *= k;
  }

  *noiseOut = noiseAmp * loud;
}

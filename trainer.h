#pragma once

#include <Arduino.h>
#include "config.h"
#include "timbre_model.h"

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
  float   in[TC_MLP_IN];
  int16_t harm[TC_N_HARM];
  int16_t noise;
  int16_t _pad;
};

class TrainSet {
public:
  void clear();
  bool add(const float *in, const float *harm, float noise);

  int  size()  const { return _n; }

  void truncate(int n) { if (n >= 0 && n < _n) _n = n; }
  bool full()  const { return _n >= TC_TRAIN_MAX; }
  const TrainSample *data() const;

  void summary() const;

  int  pitchCount() const;

private:
  int  _n      = 0;
  bool _warned = false;
};

void trainerSetProgressCallback(void (*cb)(int epoch, int total, float ce, float mae));

bool trainMlp(const TrainSet &ts, MlpWeights &out,
              int epochs = TC_TRAIN_EPOCHS,
              float lr = TC_TRAIN_LR,
              uint32_t seed = 12345,
              int progressEvery = 500);

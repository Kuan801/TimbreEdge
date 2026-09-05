// ============================================================================
//  trainer.h  -  train that MLP directly on the Teensy
//
//  Same maths as tools/train_ddsp.py, just hand-written in C++:
//    loss = cross-entropy(softmax over the 32 harmonics) + 0.3 * BCE(noise sigmoid)
//    optimiser = Adam
//
//  Why training on the MCU is possible:
//    Per sample, 2208 forward + 4288 backward ≈ 6.5k MAC. batch 128 × 6000 epoch
//    = 768k sample passes ≈ 5.0 G MAC. A Cortex-M7 @600MHz with single-cycle FMA
//    measures out at 20~40 seconds.
//    Memory: training data 215 KB + Adam/gradients 28 KB + weights 9 KB ≈ 252 KB;
//    the Teensy 4.1 has 512 KB of OCRAM, so it fits.
//
//  The audio ISR keeps running throughout training (it is an interrupt, higher
//  priority than loop()), so there are no dropouts -- you just cannot play during
//  those few tens of seconds.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "timbre_model.h"

// One training sample = 84 bytes.
//
// The harmonic targets use "square-root companding + int16" rather than float32:
//   32 floats would be 128 bytes, bloating the training set to 379 KB, which does
//   not fit; and after square-root companding the relative resolution at small
//   amplitudes (high harmonics are often down at 0.001) is 0.1%, an order of
//   magnitude better than truncating float32 straight to int16.
//   The quantisation noise is far below the measured 0.0002 convergence error, so
//   it does not hurt training quality.
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
  int16_t harm[TC_N_HARM];      // 64 bytes, normalised (sums to ≈ 1 once decoded)
  int16_t noise;                //  2 bytes
  int16_t _pad;                 //  2 bytes (keeps the 4-byte alignment)
};

class TrainSet {
public:
  void clear();
  bool add(const float *in, const float *harm, float noise);   // Returns false when full

  int  size()  const { return _n; }

  // Roll back to a given size. Needed for continuous sampling: the analysis and
  // the "add to the training set" are the same pass, and the verdict only lands
  // after the sample is already in -- so it has to be retractable.
  // Backwards only, never forwards, so it cannot conjure up data.
  void truncate(int n) { if (n >= 0 && n < _n) _n = n; }
  bool full()  const { return _n >= TC_TRAIN_MAX; }
  const TrainSample *data() const;

  // Print what has been collected so far (how many samples, which pitches covered)
  void summary() const;
  // How many distinct pitches are covered (grouped by the input feature in[0])
  int  pitchCount() const;

private:
  int  _n      = 0;
  bool _warned = false;
};

// Training blocks (tens of seconds), so set a progress callback to let the OLED
// show convergence live. Pass nullptr to cancel.
void trainerSetProgressCallback(void (*cb)(int epoch, int total, float ce, float mae));

// Train. Returns false if there is not enough data or it diverges.
//   progressEvery: report progress every N epochs (0 = no reporting)
bool trainMlp(const TrainSet &ts, MlpWeights &out,
              int epochs = TC_TRAIN_EPOCHS,
              float lr = TC_TRAIN_LR,
              uint32_t seed = 12345,
              int progressEvery = 500);

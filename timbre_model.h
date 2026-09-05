// ============================================================================
//  timbre_model.h  -  DDSP-lite timbre model
//
//  This is the "machine learning" part, but deliberately kept tiny, because it has to run
//  inference once every 2.9 ms for every sounding note:
//
//      input (4):   [ pitch(log), loudness, normalized time since attack, released? ]
//      hidden:      32 -> 32   (tanh)
//      output (33): 32 partial weights (softmax) + 1 noise gain (sigmoid)
//
//  2305 float parameters ≈ 9.0 KB (2208 weights + 97 biases), about 2.2k MAC per inference.
//  6 voices × 344 blocks per second ≈ 4.6 M MAC/s (6.1 M for 8 voices),
//  under 2% CPU on a Teensy 4.1 (600 MHz, FPU).
//
//  ★ With no MODEL.BIN it falls back to keyframe interpolation automatically; the timbre is
//    just as usable, it only loses the nonlinear adaptation to loudness/pitch.
//
//  ★ Either way, "spectral envelope (formant) correction" is applied at the end:
//    formant positions stay put when transposing, and that is the key to the synth not
//    turning into a chipmunk across the canon's whole range.
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
  // Timbre bank: every note picks the nearest sample point of its own, keeping the transposition within a semitone
  void setBank(const ProfileBank *b) { _bank = b; }
  const InstrumentProfile *profileFor(float f0Play) const {
    if (_bank && _bank->n > 0) { const InstrumentProfile *q = _bank->get(f0Play); if (q) return q; }
    return _p;
  }
  bool loadWeights(const char *path = TC_MODEL_PATH);
  void unloadWeights() { _hasMlp = false; }

  // Apply straight after on-device training (no need to write a file and read it back first)
  void adoptWeights(const MlpWeights &w);
  // Save the current weights as MODEL.BIN, in exactly the format the Python trainer produces
  bool saveWeights(const MlpWeights &w, const char *path = TC_MODEL_PATH) const;

  bool hasMlp()   const { return _hasMlp; }

  // How much the MLP counts in the final timbre. The compile-time default is TC_MLP_BLEND, but
  // it can be changed at runtime -- serial 'k' uses this for live A/B without recompiling.
  void  setBlend(float b) { _blend = tc_clampf(b, 0.0f, 1.0f); }
  float blend() const { return _blend; }

  // "Is the MLP actually affecting the sound right now?" Weights loaded but blend 0 means no --
  // use this for the status display, not hasMlp(), or the screen claims an MLP effect there isn't.
  bool  mlpActive() const { return _hasMlp && _blend > 0.0f; }
  bool ready()    const { return (_bank && _bank->n > 0) || (_p && _p->valid); }
  const InstrumentProfile *profile() const { return _p; }

  // Produce the partial amplitudes for one instant.
  //   f0Play  : fundamental to play (Hz)
  //   loud    : 0..1 current envelope loudness (velocity included)
  //   tNorm   : 0..1 position on the log time axis (computed with tc_timeWarp)
  //   released: whether release has been entered
  //   ampOut  : length nPartials, returns *linear amplitudes*, spectral envelope correction and anti-aliasing already applied.
  //             The first TC_N_HARM come from the model, the rest are extrapolated from the spectral envelope.
  //   noiseOut: gain of the noise layer
  //   nPartials: how many partials this pitch runs (computed with tc_partialCount)
  void harmonics(const InstrumentProfile *prof, float f0Play, float loud,
                 float tNorm, bool released,
                 float *ampOut, float *noiseOut, int nPartials) const;

  // Actual partial frequency (inharmonicity included), h counting from 0
  float harmonicHz(const InstrumentProfile *prof, float f0Play, int h) const;

private:
  const InstrumentProfile *_p = nullptr;
  const ProfileBank       *_bank = nullptr;
  // The weights are held inline (7 KB). An early version had them file-scope static, so several
  // TimbreModel instances shared one set of weights -- harmless in firmware (only one instance),
  // but the simulator comparing two models at once would silently get the same set. A member fixes it.
  MlpWeights  _w;
  bool        _hasMlp = false;
  float       _blend  = TC_MLP_BLEND;

  void  runMlp(const float *in, float *out) const;
  void  keyframeLookup(const InstrumentProfile *prof, float tNorm,
                       bool released, float *out) const;
};

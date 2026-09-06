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

  void setBank(const ProfileBank *b) { _bank = b; }
  const InstrumentProfile *profileFor(float f0Play) const {
    if (_bank && _bank->n > 0) { const InstrumentProfile *q = _bank->get(f0Play); if (q) return q; }
    return _p;
  }
  bool loadWeights(const char *path = TC_MODEL_PATH);
  void unloadWeights() { _hasMlp = false; }

  void adoptWeights(const MlpWeights &w);

  bool saveWeights(const MlpWeights &w, const char *path = TC_MODEL_PATH) const;

  bool hasMlp()   const { return _hasMlp; }

  void  setBlend(float b) { _blend = tc_clampf(b, 0.0f, 1.0f); }
  float blend() const { return _blend; }

  bool  mlpActive() const { return _hasMlp && _blend > 0.0f; }
  bool ready()    const { return (_bank && _bank->n > 0) || (_p && _p->valid); }
  const InstrumentProfile *profile() const { return _p; }

  void harmonics(const InstrumentProfile *prof, float f0Play, float loud,
                 float tNorm, bool released,
                 float *ampOut, float *noiseOut, int nPartials) const;

  float harmonicHz(const InstrumentProfile *prof, float f0Play, int h) const;

private:
  const InstrumentProfile *_p = nullptr;
  const ProfileBank       *_bank = nullptr;

  MlpWeights  _w;
  bool        _hasMlp = false;
  float       _blend  = TC_MLP_BLEND;

  void  runMlp(const float *in, float *out) const;
  void  keyframeLookup(const InstrumentProfile *prof, float tNorm,
                       bool released, float *out) const;
};

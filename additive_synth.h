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

  void setVibrato(float maxCents, float hz) { _vibMaxCents = maxCents; _vibHz = hz; }

  void setPitchBend(float semitones) {
    _bendMul = powf(2.0f, tc_clampf(semitones, -24.0f, 24.0f) / 12.0f);
  }

  void setModDepth(float cents) { _modCents = tc_clampf(cents, 0.0f, 100.0f); }

  enum NoteResult : uint8_t {
    NOTE_OK = 0,
    NOTE_NO_MODEL,
    NOTE_NO_TIMBRE,
    NOTE_NO_VOICE
  };
  NoteResult noteOn(float midi, float vel = 1.0f, float pan = 0.5f);

  bool hasTimbre() const { return _model && _model->ready(); }
  void noteOff(float midi);
  void allNotesOff();
  int  activeVoices() const;

  virtual void update(void);

private:

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

    int      nPart = TC_N_HARM;
    uint32_t phase[TC_N_PARTIAL];
    uint32_t baseInc[TC_N_PARTIAL];
    float    amp[TC_N_PARTIAL];
    float    ampStep[TC_N_PARTIAL];

    float    onsetT[TC_N_PARTIAL];

    float    shimPhase[TC_N_PARTIAL];
    float    shimInc[TC_N_PARTIAL];
    float    shimDepth = 0.0f;

    float    noise = 0.0f, noiseStep = 0.0f;

    float    nbLpB[3] = {1.0f, 0.0f, 0.0f}, nbLpA[2] = {0.0f, 0.0f};
    float    nbHpB[3] = {1.0f, 0.0f, 0.0f}, nbHpA[2] = {0.0f, 0.0f};
    float    nbLpZ[TC_NOISE_LP_STAGES][2] = {{0.0f, 0.0f}};
    float    nbHpZ[2] = {0.0f, 0.0f};
    float    noiseNrm = 1.0f;
    float    noiseFLo = 0.0f, noiseFHi = 0.0f;
    float    jitterSigma = 0.0f;
    float    jitFrac = 0.9f;
    float    atkJitVar = 0.0f, atkJitSigma = 0.0f;
    float    jit[TC_N_PARTIAL] = {0};
    float    noiseAtk = 0.0f;
    uint32_t rng   = 0x13579BDFu;

    float    vibPhase = 0.0f;
    float    vibCents = 0.0f;
    float    vibHz    = 4.8f;

    float    rCoef  = 0.999f;
    float    refDur = 1.0f;
    float    holdNorm = 1.0f;

    float    tailCoef = 1.0f;
    float    envTail  = 0.0f;
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

// ============================================================================
//  additive_synth.h  -  custom AudioStream: TC_N_VOICES-voice additive synth
//
//  Each voice = 16 sine partials + 1 filtered noise layer + ADSR envelope
//  Partial amplitudes are updated once per block (2.9 ms) by TimbreModel and ramped
//  linearly within the block, so it sounds like a continuous change, not a staircase.
//
//  CPU estimate: 16 partials × 128 samples × 6 voices = 12288 table interpolations / block
//                ≈ 0.12 M cycles / 2.9 ms, about 7% CPU on a Teensy 4.1 (600 MHz).
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
  // Vibrato depth comes from the profile (measured 0 on piano, so none is added). What is set
  // here is the upper limit and the rate; cents < 0 means leave it entirely to the profile.
  void setVibrato(float maxCents, float hz) { _vibMaxCents = maxCents; _vibHz = hz; }

  // Pitch bend wheel, in semitones (the Keystation's <PB / PB> buttons).
  // Applies to already-sounding notes too, so it's a global multiplier, not fixed at noteOn.
  void setPitchBend(float semitones) {
    _bendMul = powf(2.0f, tc_clampf(semitones, -24.0f, 24.0f) / 12.0f);
  }
  // Mod wheel: depth (in cents) added *on top of* the vibrato measured in the profile.
  // Added rather than substituted, so MOD still does something on piano (which measured 0).
  void setModDepth(float cents) { _modCents = tc_clampf(cents, 0.0f, 100.0f); }

  // Result of noteOn. It used to be void, so every kind of failure was "silently do nothing" --
  // when a key made no sound there was no way to find out why, hence the reported result.
  enum NoteResult : uint8_t {
    NOTE_OK = 0,
    NOTE_NO_MODEL,      // setModel() not called yet
    NOTE_NO_TIMBRE,     // No timbre loaded (no BANK.BIN / PROFILE.BIN on the SD card)
    NOTE_NO_VOICE       // All voices busy and none could be stolen
  };
  NoteResult noteOn(float midi, float vel = 1.0f, float pan = 0.5f);

  bool hasTimbre() const { return _model && _model->ready(); }
  void noteOff(float midi);
  void allNotesOff();
  int  activeVoices() const;

  virtual void update(void);

private:
  // The envelope just follows the curve measured in the profile, so two states are enough.
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

    int      nPart = TC_N_HARM;          // How many partials this pitch actually runs (decided automatically from f0)
    uint32_t phase[TC_N_PARTIAL];
    uint32_t baseInc[TC_N_PARTIAL];
    float    amp[TC_N_PARTIAL];
    float    ampStep[TC_N_PARTIAL];

    // Asynchronous attack: each partial's own entry time (seconds) and gate state
    float    onsetT[TC_N_PARTIAL];
    // shimmer: an independent slow wobble per partial
    float    shimPhase[TC_N_PARTIAL];
    float    shimInc[TC_N_PARTIAL];
    float    shimDepth = 0.0f;

    float    noise = 0.0f, noiseStep = 0.0f;
    // Noise band-pass: TC_NOISE_LP_STAGES biquad low-pass stages + one biquad high-pass.
    // Not cascaded one-pole recursions -- once the corner is high they degenerate to y = x, i.e. no low-pass at all.
    float    nbLpB[3] = {1.0f, 0.0f, 0.0f}, nbLpA[2] = {0.0f, 0.0f};
    float    nbHpB[3] = {1.0f, 0.0f, 0.0f}, nbHpA[2] = {0.0f, 0.0f};
    float    nbLpZ[TC_NOISE_LP_STAGES][2] = {{0.0f, 0.0f}};
    float    nbHpZ[2] = {0.0f, 0.0f};
    float    noiseNrm = 1.0f;      // Filter RMS compensation, measured at noteOn
    float    noiseFLo = 0.0f, noiseFHi = 0.0f;   // Band-pass edges actually used (for debugging)
    float    jitterSigma = 0.0f;   // Standard deviation of the partial amplitude jitter
    float    jitFrac = 0.9f;       // Fraction of the aperiodic energy that goes through the jitter
    float    atkJitVar = 0.0f, atkJitSigma = 0.0f;   // Extra jitter during the attack
    float    jit[TC_N_PARTIAL] = {0};
    float    noiseAtk = 0.0f;            // Extra gain for the attack noise burst
    uint32_t rng   = 0x13579BDFu;

    float    vibPhase = 0.0f;
    float    vibCents = 0.0f;
    float    vibHz    = 4.8f;      // Vibrato rate (from the profile)

    // Envelope parameters carried in from the profile
    float    rCoef  = 0.999f;      // Decay after release (per block)
    float    refDur = 1.0f;        // Length of the source material, used to convert the warped time axis
    float    holdNorm = 1.0f;      // The envelope stops falling here
    // How much to keep multiplying by per block once the measured envelope curve runs out.
    //
    // Decaying instruments (piano/guitar/plucked) take it from the profile's sustainDecayPerSec;
    // sustaining ones use 1.0, i.e. hold steady (the bow/breath is still going).
    //
    // Without this the note freezes at the height of loud[31] and never falls further --
    // measured piano loud[] only drops to -24 dB before it ends, so every note sat flat at
    // -24 dB and sounded like an organ. The shorter the recording the more obvious it is (with
    // a 1.79 s sample, the canon's half notes finish the envelope before they are even released).
    float    tailCoef = 1.0f;
    float    envTail  = 0.0f;      // Envelope value after the curve runs out; 0 = not into the tail yet
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

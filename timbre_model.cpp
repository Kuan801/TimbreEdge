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

// tc_tanh / tc_sigmoid are defined in config.h and the trainer uses the very same ones -- this
// matters, or training with tanhf and inferring with an approximation gives a train/inference mismatch.
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

  // softmax over the first TC_N_HARM -> partial distribution; sigmoid on the last -> noise
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

  // After release the upper partials decay faster than the fundamental (behaviour common to real instruments)
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
  return f0Play * n * sqrtf(1.0f + prof->inharmonicity * n * n);   // String inharmonicity
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

  // Keyframes are the *directly measured* distribution and the most faithful thing we have; the
  // MLP is learned, so it smooths and leaks. Measured on real piano material: keyframes give
  // 51.3/35.0/9.4/1.3/1.4/0.7/0.9 (ground truth 53.8/34.5/8.7/1.5/1.4/0.0/0.0), almost exact,
  // while the MLP pushes h2 down and invents energy in h4~h7 that the real instrument doesn't have.
  //
  // So it became a log-domain blend of "keyframes as the base, MLP as a correction only":
  //   BLEND = 0 trusts the keyframes entirely, 1 trusts the MLP entirely.
  // The MLP keeps what it is genuinely worth (fine tracking of loudness/pitch) yet cannot wreck the basic timbre.
  keyframeLookup(prof, tNorm, released, raw);

  // The guard is mlpActive(), not _hasMlp: with blend at 0 the whole inference is cancelled out by
  // the blend expression (exp(lk + 0*(lm-lk)) == exp(lk)), so running it is pure waste.
  // Measured, that waste is not small -- 2208 MAC per voice per block, plus 32 logf/expf pairs
  // (tens of cycles each on the M7); about 3% CPU at 6 voices spent on something with no effect.
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

  // ---- Spectral envelope (formant) correction + anti-aliasing ------------
  // What the raw keyframe/MLP gives is the partial distribution "at the reference f0, _p->f0".
  // Moving to f0Play, each partial looks up the envelope gain at its *new absolute frequency* and
  // divides out its gain at the reference pitch -- formants stay put, so the timbre doesn't change voice.
  const float nyq = TC_SAMPLE_RATE * TC_NYQUIST_GUARD;
  float energy = 0.0f;

  // The first TC_N_HARM partials: straight from the model
  int nModel = (nPartials < TC_N_HARM) ? nPartials : TC_N_HARM;
  for (int h = 0; h < nModel; h++) {
    float fNew = harmonicHz(prof, f0Play, h);
    if (fNew >= nyq) { ampOut[h] = 0.0f; continue; }

    // Cosine fade-out for partials near Nyquist, to avoid harsh aliasing
    float roll = 1.0f;
    if (fNew > nyq * 0.8f) roll = 0.5f * (1.0f + cosf((float)M_PI * (fNew - nyq * 0.8f) / (nyq * 0.2f)));

#if TC_TRANSPOSE_RESAMPLE
    // Which source partial the target's h-th lands on (continuous, counting from 0).
    // Ask directly "how loud is the source at this absolute frequency" -- the formants stay put,
    // and what you get is the time trajectory of **that frequency**, not of partial h. See config.h.
    const float hSrcF = fNew / prof->f0 - 1.0f;
    float a;
    if (hSrcF <= (float)(TC_N_HARM - 1)) {
      const int   i0 = (hSrcF > 0.0f) ? (int)hSrcF : 0;
      const int   i1 = (i0 + 1 < TC_N_HARM) ? (i0 + 1) : (TC_N_HARM - 1);
      const float fr = tc_clampf(hSrcF - (float)i0, 0.0f, 1.0f);
      // Log-domain interpolation: the distribution's dynamic range is huge, so linear interpolation would be dominated by the loudest partial
      a = expf((1.0f - fr) * logf(raw[i0] + 1e-6f) + fr * logf(raw[i1] + 1e-6f));
    } else {
      // Already past the 32 partials we measured, so all that's left is extrapolating outward from the last one with the envelope
      const float fLast = prof->f0 * (float)TC_N_HARM;
      const float ratio = specEnvGain(*prof, fNew) / (specEnvGain(*prof, fLast) + 1e-6f);
      a = raw[TC_N_HARM - 1] * tc_clampf(ratio, 0.0f, 1.5f);
    }
    // Log-domain blend with the old approach, weight TC_TRANSPOSE_RESAMPLE (1 = all resampling).
    // The knob stays because each has its own weakness: resampling gets the time trajectory at the
    // correct frequency, but a non-octave transposition falls between two partials and may interpolate
    // a *valley* rather than the envelope; the old smoothed envelope had no such ripple, but carried the wrong trajectory.
    {
      const float fRef = prof->f0 * (h + 1);
      float g = specEnvGain(*prof, fNew) / (specEnvGain(*prof, fRef) + 1e-6f);
      g = tc_clampf(g, 0.05f, 4.0f);        // Don't let the envelope correction run away
      const float aOld = raw[h] * g;
      // The weight follows the transposition distance.
      //
      // Resampling costs interpolation error: when the target partial lands between two source
      // partials, what you pick up may be a *valley* instead of the envelope. That error scales with
      // the fractional part of the index, which in turn scales with the deviation of the pitch ratio
      // -- so the less you transpose, the higher the cost and the smaller the benefit.
      //
      // Measured: for notes the timbre bank does cover (playing pitch only a few cents from the
      // profile, yet the index of partial 30 already off by 0.12), running them through resampling
      // took the guitar's mid-range LSD from 0.59 to 0.85 -- pure interpolation ripple, bought nothing.
      // None at all within one semitone, full above four semitones.
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
    g = tc_clampf(g, 0.05f, 4.0f);          // Don't let the envelope correction run away
    ampOut[h] = raw[h] * g * roll;
#endif
    energy   += ampOut[h] * ampOut[h];
  }

  // Partial 33 and up: extrapolated from the *measured spectral envelope* instead.
  // Past 32 partials a real recording is mostly buried in noise, so modelling each one is learning
  // noise; the envelope is far more robust. Low notes only stay bright thanks to this: D2 topped out at 1184 Hz before, now it reaches 4.7 kHz.
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

  // Normalize by *energy* (L2), not by *sum of amplitudes* (L1).
  // This matters: on instruments like piano the upper partials decay faster than the fundamental, so
  // the spectrum keeps concentrating, and with L1 normalization the energy actually rises as it
  // concentrates, so it never sounds like it decays. With L2 the output RMS follows the ADSR faithfully.
  // ---- Energy split between partials and noise ---------------------------
  //
  // raw[TC_N_HARM] is the analyzer's measured "fraction of the total energy that is aperiodic".
  //
  // An early version used it directly as an amplitude multiplier -- that is a units error. An energy
  // ratio of 0.004 corresponds to an amplitude ratio of sqrt(0.004) = 0.063, a factor of 16 (24 dB)
  // out. Measured inter-partial noise floor on flute: real -72.6 dB, synthesized -99.5 dB, exactly
  // that order. Barely noticeable on piano/violin (partials far above the noise), but the breath is
  // what characterizes a flute; without that floor it turns into an organ.
  const float noiseFrac = tc_clampf(raw[TC_N_HARM], 0.0f, 0.9f);
  const float harmAmp   = sqrtf(1.0f - noiseFrac);
  // Only the share of the residual that falls above 5*f0 goes through the wideband noise layer; the
  // rest comes from additive_synth's partial jitter, so the energy lands at the right frequencies. Ratio from measurement.
  const float noiseAmp  = sqrtf(noiseFrac * tc_clampf(prof->noiseHighFrac, 0.0f, 0.9f));

  float rms = sqrtf(energy);
  if (rms > 1e-9f) {
    // sqrt(2) because a sine's RMS is 1/sqrt(2) of its amplitude: this makes the output RMS exactly equal loud
    float k = loud * 1.41421356f * harmAmp / rms;
    for (int h = 0; h < nPartials; h++) ampOut[h] *= k;
  }

  *noiseOut = noiseAmp * loud;
}

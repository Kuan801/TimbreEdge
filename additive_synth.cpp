#include "additive_synth.h"

// --------------------------------------------------------- sine table ------
// 1024 points + 1 wrap guard; after linear interpolation THD is about -78 dB,
// still good enough for a 64-harmonic additive sum.
static float  sSine[TC_SINE_TBL_SIZE + 1];
static bool   sSineReady = false;

static void buildSine() {
  if (sSineReady) return;
  for (int i = 0; i <= TC_SINE_TBL_SIZE; i++)
    sSine[i] = sinf(2.0f * (float)M_PI * i / TC_SINE_TBL_SIZE);
  sSineReady = true;
}

static inline float sineLookup(uint32_t phase) {
  uint32_t idx  = phase >> (32 - TC_SINE_TBL_BITS);          // 0..1023
  float    frac = (float)(phase & ((1u << (32 - TC_SINE_TBL_BITS)) - 1))
                  * (1.0f / (float)(1u << (32 - TC_SINE_TBL_BITS)));
  float a = sSine[idx], b = sSine[idx + 1];
  return a + (b - a) * frac;
}

// ------------------------------------------------------- soft clip --------
// 0 ~ 0.75 FS is perfectly linear (dynamics untouched); only peaks above the
// threshold get squeezed into 1.0 FS with tanh.
// The early version used x/(1+|x|/k), which compressed the whole signal — the
// measurements showed the sustain lifted and the attack flattened, i.e. half the
// overall dynamic range gone. Hence this version with a knee.
#define SC_THRESH 24575.0f            // 0.75 * 32767
#define SC_RANGE  8192.0f             // 32767 - SC_THRESH
static inline float softClip(float x) {
  float a = fabsf(x);
  if (a <= SC_THRESH) return x;
  float t = (a - SC_THRESH) / SC_RANGE;
  float y = SC_THRESH + SC_RANGE * tanhf(t);
  return (x < 0.0f) ? -y : y;
}

static inline uint32_t hzToInc(float hz) {
  if (hz <= 0.0f) return 0;
  return (uint32_t)(hz * (4294967296.0f / TC_SAMPLE_RATE));
}

// One scratch buffer per voice (used inside the audio ISR — keep it off the stack)
DMAMEM static float sVoiceBuf[TC_BLOCK];
DMAMEM static float sAccL[TC_BLOCK];
DMAMEM static float sAccR[TC_BLOCK];

// ---------------------------------------------------------------------------
AudioSynthAdditive::AudioSynthAdditive() : AudioStream(0, NULL) {
  buildSine();
  for (int i = 0; i < TC_N_VOICES; i++) {
    _v[i].rng = 0x13579BDFu ^ (uint32_t)(i * 2654435761u);
    for (int h = 0; h < TC_N_PARTIAL; h++) {
      _v[i].phase[h]     = 0;
      _v[i].amp[h]       = 0.0f;
      _v[i].ampStep[h]   = 0.0f;
      _v[i].baseInc[h]   = 0;
      _v[i].onsetT[h]    = 0.0f;
      _v[i].shimPhase[h] = 0.0f;
      _v[i].shimInc[h]   = 0.0f;
    }
  }
}

// ---------------------------------------------------------------------------
int AudioSynthAdditive::allocVoice(float midi) {
  // 1) same pitch already sounding -> just retrigger
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE && fabsf(_v[i].midi - midi) < 0.01f) return i;
  // 2) a free one
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage == IDLE) return i;
  // 3) steal the oldest releasing voice, failing that the oldest voice
  int best = -1;
  uint32_t oldest = 0xFFFFFFFFu;
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage == RELEASE && _v[i].age < oldest) { oldest = _v[i].age; best = i; }
  if (best >= 0) return best;
  oldest = 0xFFFFFFFFu;
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].age < oldest) { oldest = _v[i].age; best = i; }
  return best;
}

// ---------------------------------------------------------------------------
AudioSynthAdditive::NoteResult
AudioSynthAdditive::noteOn(float midi, float vel, float pan) {
  if (!_model) return NOTE_NO_MODEL;
  // Every note picks the sampled pitch closest to it, keeping the transposition distance under a semitone
  const InstrumentProfile *p = _model->profileFor(tc_midiToHz(midi));
  if (!p || !p->valid) return NOTE_NO_TIMBRE;

  int i = allocVoice(midi);
  if (i < 0) return NOTE_NO_VOICE;
  Voice &v = _v[i];

  v.midi  = midi;
  v.f0    = tc_midiToHz(midi);
  v.vel   = tc_clampf(vel, 0.05f, 1.0f);
  v.pan   = tc_clampf(pan, 0.0f, 1.0f);
  v.tSec  = 0.0f;
  v.stage = PLAYING;
  // The tail state must be cleared. When a voice is reused, leaving it carries the
  // previous note's tail level over and the new note starts from a tiny amplitude.
  v.envTail = 0.0f;
  v.age   = _ageCounter++;
  v.vibPhase = 0.0f;

  // --- envelope: play back the measured profile curve ----------------------
  // No more approximating with four A/D/S/R parameters. A piano's two-stage decay
  // or a violin's swell simply cannot be expressed by a parametric model, but the
  // measured curve is right by construction.
  const float bs = TC_BLOCK_SEC;
  v.prof     = p;
  v.refDur   = fmaxf(p->noteDur, 0.2f);
  v.holdNorm = (p->envHoldNorm > 0.01f) ? p->envHoldNorm : 1.0f;
  v.rCoef    = expf(-bs / fmaxf(p->release * 0.4f, 0.02f));

  // What to do once the envelope curve runs out.
  //
  // Decaying:   keep falling at the measured decay rate. sustainDecayPerSec is the
  //             "amplitude factor per second", so per block it is that raised to
  //             the (bs/1 second) power.
  // Sustaining: hold flat (the bow / breath is still going), i.e. 1.0.
  //
  // This term did not exist before and the note just sat at the loud[31] level.
  // A real piano's loud[] only falls to −24 dB, so every note plateaued at −24 dB
  // and the canon sounded like a pipe organ. The comment on sustainDecayPerSec
  // always said "this is what gives the long natural decay", but the synth never
  // read it — this is where it finally gets hooked up.
  {
    const float perSec = p->sustainDecayPerSec;
    v.tailCoef = (perSec > 0.0f && perSec < 0.999f) ? powf(perSec, bs) : 1.0f;
  }

  // Harmonic count follows the pitch: fill up to Nyquist so low notes aren't dull
  v.nPart = tc_partialCount(v.f0);

  // Harmonic frequencies (including inharmonicity) and the noise filter coefficients
  for (int h = 0; h < v.nPart; h++)
    v.baseInc[h] = hzToInc(_model->harmonicHz(p, v.f0, h));

  // The noise layer is a band-pass, not a low-pass.
  //
  // The old version low-passed the noise at 5*f0 (only 2.2 kHz for a flute A4),
  // but real breath / bow noise is broadband. Squeezing the same total energy
  // into 1/9 of the bandwidth lifts every bin by 10*log10(9)=9.6 dB; measured,
  // the flute's between-harmonic floor came out 11 dB above the real thing —
  // right energy, wrong bandwidth.
  //
  //   Lower edge fLo = 5*f0: residual below this is handled by harmonic jitter
  //                    (the sidebands sit on the harmonics, which is where they
  //                    belong); the broadband layer only takes the part above 5*f0.
  //   Upper edge fHi = the frequency at which the instrument's own spectral
  //                    envelope has fallen to peak -TC_NOISE_ROLL_DB.
  //
  // Why the upper edge cannot be 8*f0 (this is what caused the sizzle on note
  // onsets): 8*f0 only looks at pitch, not at whether the instrument still has
  // any energy up there. A piano F#5 puts the noise layer at 3.7~5.9 kHz, while
  // the piano's spectral envelope is already -40 dB at 2.5 kHz and -67 dB at
  // 5 kHz — measured in situ on the attack, above 5 kHz the noise layer was
  // 15~23 dB louder than the piano's own harmonics. Nothing masks it.
  // With specEnv, the same measurement went from "15.2 dB too much" to "5.1 dB
  // too little". specEnv was already in the profile, so this change needs no
  // re-analysis.
#ifdef TC_NOISE_FIXED_BAND
  // Experiment: band-pass over a fixed absolute band (not tracking pitch), lower edge still never below f0
  float fLo = tc_clampf(fmaxf((float)TC_NOISE_FLO, v.f0), 150.0f, 6000.0f);
  float fHi = tc_clampf((float)TC_NOISE_FHI, fLo * 1.5f, 9000.0f);
#else
  float fRoll = TC_SPECENV_FMAX;
  {
    float pk = -1e30f;
    for (int q = 0; q < TC_SPECENV_PTS; q++) if (p->specEnv[q] > pk) pk = p->specEnv[q];
    const float th = pk - TC_NOISE_ROLL_DB;
    for (int q = TC_SPECENV_PTS - 1; q >= 0; q--) {
      if (p->specEnv[q] > th) {
        fRoll = TC_SPECENV_FMIN * powf(TC_SPECENV_FMAX / TC_SPECENV_FMIN,
                                       (float)q / (float)(TC_SPECENV_PTS - 1));
        break;
      }
    }
  }
  float fHi = tc_clampf(fRoll, 900.0f, 8000.0f);
  // The lower edge must not exceed half the upper edge. Up high, 5*f0 is often
  // already outside the instrument's spectrum, and forcing it collapses the
  // band-pass into a shell with almost no pass-band, after which the RMS
  // normalisation below amplifies the little that is left several times over —
  // that was the fate of the previous "3 kHz corner cap per stage" version.
  float fLo = tc_clampf(fminf(v.f0 * 5.0f, fHi * 0.5f), 200.0f, 4000.0f);
#endif
  v.noiseFLo = fLo;
  v.noiseFHi = fHi;

  // Band-pass built from biquads (RBJ, Q = 1/sqrt(2)): TC_NOISE_LP_STAGES
  // low-pass stages plus one high-pass. The one-pole recursion y += c*(x-y)
  // degenerates to y = x as c -> 1, i.e. no low-pass at all up high; a bilinear
  // biquad has no such problem, and only a 12 dB/oct roll-off per stage is steep
  // enough to hold back the leakage above 5 kHz.
  {
    const float Q = 0.70710678f;
    {
      const float w = 2.0f * (float)M_PI * fHi / TC_SAMPLE_RATE;
      const float cw = cosf(w), al = sinf(w) / (2.0f * Q), a0 = 1.0f + al;
      v.nbLpB[0] = (1.0f - cw) * 0.5f / a0;
      v.nbLpB[1] = (1.0f - cw) / a0;
      v.nbLpB[2] = v.nbLpB[0];
      v.nbLpA[0] = (-2.0f * cw) / a0;
      v.nbLpA[1] = (1.0f - al) / a0;
    }
    {
      const float w = 2.0f * (float)M_PI * fLo / TC_SAMPLE_RATE;
      const float cw = cosf(w), al = sinf(w) / (2.0f * Q), a0 = 1.0f + al;
      v.nbHpB[0] = (1.0f + cw) * 0.5f / a0;
      v.nbHpB[1] = -(1.0f + cw) / a0;
      v.nbHpB[2] = v.nbHpB[0];
      v.nbHpA[0] = (-2.0f * cw) / a0;
      v.nbHpA[1] = (1.0f - al) / a0;
    }
  }
  for (int k = 0; k < TC_NOISE_LP_STAGES; k++) { v.nbLpZ[k][0] = 0.0f; v.nbLpZ[k][1] = 0.0f; }
  v.nbHpZ[0] = v.nbHpZ[1] = 0.0f;
  // Deriving the filter's actual RMS gain analytically is error-prone, so just
  // measure it over 1024 samples — once per note, so the cost is negligible.
  {
    uint32_t r = v.rng ^ 0x5A5A5A5Au;
    float lz[TC_NOISE_LP_STAGES][2] = {{0.0f, 0.0f}};
    float hz[2] = {0.0f, 0.0f};
    float acc = 0.0f;
    for (int i = 0; i < 1024; i++) {
      r = r * 1664525u + 1013904223u;
      float x = ((int32_t)(r >> 8) * (1.0f / 8388608.0f)) - 1.0f;
      for (int k = 0; k < TC_NOISE_LP_STAGES; k++) {
        const float y = v.nbLpB[0] * x + lz[k][0];
        lz[k][0] = v.nbLpB[1] * x - v.nbLpA[0] * y + lz[k][1];
        lz[k][1] = v.nbLpB[2] * x - v.nbLpA[1] * y;
        x = y;
      }
      {
        const float y = v.nbHpB[0] * x + hz[0];
        hz[0] = v.nbHpB[1] * x - v.nbHpA[0] * y + hz[1];
        hz[1] = v.nbHpB[2] * x - v.nbHpA[1] * y;
        x = y;
      }
      if (i >= 256) acc += x * x;          // first 256 are warm-up, not counted
    }
    v.noiseNrm = 1.0f / (sqrtf(acc / 768.0f) + 1e-6f);
  }
  // The attack's broadband layer and the sustain share the same band-pass: where
  // both land is already decided by measurement, so there is no need for another
  // hard-coded 9 kHz low-pass just for the attack.
  // attackNoise / noiseGain are both energy ratios; take the square root to get an
  // amplitude ratio before using them directly as a multiplier (same as the note
  // in timbre_model.cpp).
  // The extra aperiodic energy of the attack is split by the measured landing
  // point too, into a high-frequency broadband part and a part that sits on the
  // harmonics — the same logic as the sustain, so no per-instrument branches.
  {
    float extra = tc_clampf(p->attackNoise - p->noiseGain, 0.0f, 0.9f);   // energy ratio
#ifdef TC_DBG_NO_ATK
    extra = 0.0f;                    // debug: drop all of the extra attack energy
#endif
    float hi    = tc_clampf(p->attackHighFrac, 0.0f, 0.9f);
    v.noiseAtk  = sqrtf(extra * hi);              // broadband layer (amplitude)
    v.atkJitVar = extra * (1.0f - hi);            // jitter layer (variance; comp is multiplied in later)
  }

  // The sustain's aperiodic energy is split in two (see the 2b' note in update()):
  //   the part measured down low goes to harmonic jitter — the sidebands sit on
  //     the harmonics, which is where they belong
  //   only the rest uses the broadband noise layer — real instruments do have a
  //     little broadband air noise
  {
    float nf = tc_clampf(p->noiseGain, 0.0f, 0.9f);
    // For the synthesised aperiodic ratio, as measured by the period difference,
    // to equal the noiseGain the analyzer measured, two layers of attenuation have
    // to be compensated back:
    //
    // (1) The period difference x[n]-x[n-T] has gain 2|sin(pi*df/f0)| on the
    //     sidebands — the closer to the carrier, the less it sees. The jitter
    //     sidebands are spread over 0~B (B = half the block rate = 172 Hz), so on
    //     average mean(sin^2) = 0.5 - sin(2*pi*B/f0)/(4*pi*B/f0) and the measured
    //     ratio = 2 * sigma^2 * mean(sin^2). This term depends on pitch: a flute
    //     A4 needs 1.35x, a piano C4 only 0.83x, so a fixed constant is wrong at
    //     both ends.
    // (2) The linear interpolation inside the block low-passes once more, another
    //     1.1x measured.
    const float blkNyq = 0.5f / TC_BLOCK_SEC;          // 172 Hz
    const float xb = 2.0f * (float)M_PI * blkNyq / v.f0;
    float meanSin2 = 0.5f - ((fabsf(xb) > 1e-4f) ? sinf(xb) / (2.0f * xb) : 0.5f);
    meanSin2 = tc_clampf(meanSin2, 0.05f, 0.5f);
    const float comp = 1.0f / (2.0f * meanSin2 * 0.83f);   // 0.83 = the extra attenuation from interpolation
    // The split between harmonic jitter and broadband noise comes from where the
    // measured residual lands, not from a fixed value: low-frequency residual
    // sitting on the harmonics -> jitter; residual above 5*f0 -> broadband layer.
    // Measured, 15% goes broadband for the flute and 60% for the violin, which is
    // exactly the difference between breath noise and bow noise.
    float hiFrac = tc_clampf(p->noiseHighFrac, 0.0f, 0.9f);
    v.jitFrac = 1.0f - hiFrac;
    v.jitterSigma = sqrtf(nf * v.jitFrac * comp * TC_JITTER_CAL);   // see the note in config.h

    // Cap on the jitter depth, plus "whatever overflows goes to the broadband
    // layer".
    //
    // Once sigma exceeds 1/sqrt(3) the multiplier 1 + sigma*jit hits both ends of
    // the clamp and the jitter degenerates into a random on/off switch every
    // 2.9 ms (measured: 68% of the draws hit the wall on B5) — and that is where
    // the sizzle on note onsets comes from. See TC_JIT_SIGMA_MAX in config.h.
    //
    // The overflowing variance has to move to the broadband noise layer, not be
    // thrown away: both describe the same aperiodic energy of the attack, only in
    // different places. Throw it away and the attack ends up cleaner than the real
    // material — measured, the synthesised attack aperiodic ratio is already only
    // 0.2x the reference, and any less is further off still.
    {
      const float sigMax2 = TC_JIT_SIGMA_MAX * TC_JIT_SIGMA_MAX;
      if (v.atkJitVar * comp > sigMax2) {
        const float keep  = sigMax2 / comp;             // variance the jitter layer can still hold (energy ratio)
        const float spill = v.atkJitVar - keep;
        v.atkJitVar = keep;
        // both are energy ratios, so they add in quadrature (noiseAtk holds an amplitude)
        v.noiseAtk  = sqrtf(v.noiseAtk * v.noiseAtk + spill);
      }
      v.atkJitSigma = sqrtf(v.atkJitVar * comp);
      // measured sustain is only 0.02~0.15, far from the cap, but don't leave a path that can blow up
      if (v.jitterSigma > TC_JIT_SIGMA_MAX) v.jitterSigma = TC_JIT_SIGMA_MAX;
    }
    for (int h = 0; h < TC_N_PARTIAL; h++) v.jit[h] = 0.0f;
  }

  // Vibrato depth comes from the material: violins have it, pianos don't.
  // Adding vibrato across the board makes every instrument taste the same.
  v.vibCents = tc_clampf(p->vibratoCents, 0.0f, _vibMaxCents);
  // Vibrato rate is measured too, no longer a flat 4.8 Hz — it varies a lot
  // between instruments and players
  v.vibHz    = (p->vibratoHz > 2.0f) ? p->vibratoHz : _vibHz;

  // Asynchronous attack: carry the measured per-harmonic delays over.
  // Beyond TC_N_HARM, reuse the last harmonic's delay and extrapolate it slightly
  // with the harmonic index.
  for (int h = 0; h < v.nPart; h++) {
    if (h < TC_N_HARM) v.onsetT[h] = p->harmOnset[h];
    else               v.onsetT[h] = p->harmOnset[TC_N_HARM - 1];
  }

  // shimmer: one independent slow LFO per harmonic; the frequencies are spread out
  // so it doesn't read as a tidy vibrato.
  // The profile stores a standard deviation, and a sine's standard deviation is
  // 1/sqrt(2) of its amplitude, so multiply by sqrt(2) to make the synthesised
  // fluctuation match the measured one.
  v.shimDepth = tc_clampf(p->shimmerDepth * 1.41421356f, 0.0f, 0.30f);
  for (int h = 0; h < v.nPart; h++) {
    uint32_t r = (v.rng ^ (uint32_t)(h * 2654435761u)) * 1664525u + 1013904223u;
    float u1 = (float)(r >> 8) * (1.0f / 16777216.0f);
    r = r * 1664525u + 1013904223u;
    float u2 = (float)(r >> 8) * (1.0f / 16777216.0f);
    float hz = TC_SHIMMER_HZ_MIN + u1 * (TC_SHIMMER_HZ_MAX - TC_SHIMMER_HZ_MIN);
    v.shimInc[h]   = 2.0f * (float)M_PI * hz * TC_BLOCK_SEC;
    v.shimPhase[h] = u2 * 6.2831853f;
  }

  // Phase: putting every harmonic in phase at the instant of attack produces a
  // "click" impulse, so spread them out a little
  for (int h = 0; h < v.nPart; h++)
    v.phase[h] = (uint32_t)((h * 2654435761u) ^ (v.age * 40503u));

  if (v.env < 0.001f) {
    for (int h = 0; h < TC_N_PARTIAL; h++) { v.amp[h] = 0.0f; v.ampStep[h] = 0.0f; }
    v.noise = 0.0f; v.noiseStep = 0.0f;
  }
  return NOTE_OK;
}

// ---------------------------------------------------------------------------
void AudioSynthAdditive::noteOff(float midi) {
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE && _v[i].stage != RELEASE && fabsf(_v[i].midi - midi) < 0.01f)
      _v[i].stage = RELEASE;
}

void AudioSynthAdditive::allNotesOff() {
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE) _v[i].stage = RELEASE;
}

int AudioSynthAdditive::activeVoices() const {
  int n = 0;
  for (int i = 0; i < TC_N_VOICES; i++) if (_v[i].stage != IDLE) n++;
  return n;
}

// ---------------------------------------------------------------------------
void AudioSynthAdditive::renderVoice(Voice &v, float *dst) {
  for (int i = 0; i < TC_BLOCK; i++) dst[i] = 0.0f;

  // Vibrato: eases in only 0.25 s after the attack, the way a real player does it.
  // Depth comes from the profile.
  // The mod wheel's share is added on top, and it does not wait the 0.25 s — the
  // player pushes it and expects a response.
  float vibDepth = 0.0f;
  if (v.vibCents > 0.5f && v.tSec > 0.25f)
    vibDepth = tc_clampf((v.tSec - 0.25f) / 0.5f, 0.0f, 1.0f) * v.vibCents;
  vibDepth += _modCents;

  float vibMul = 1.0f;
  if (vibDepth > 0.0f) {
    v.vibPhase += 2.0f * (float)M_PI * v.vibHz * TC_BLOCK_SEC;
    if (v.vibPhase > 2.0f * (float)M_PI) v.vibPhase -= 2.0f * (float)M_PI;
    vibMul = powf(2.0f, (vibDepth * sinf(v.vibPhase)) / 1200.0f);
  }

  // ---- harmonics ---------------------------------------------------------
  for (int h = 0; h < v.nPart; h++) {
    float a  = v.amp[h];
    float st = v.ampStep[h];
    if (a < 1e-6f && st <= 0.0f) { v.amp[h] = 0.0f; continue; }

    uint32_t ph  = v.phase[h];
    uint32_t inc = v.baseInc[h];
    float    mul = (vibDepth > 0.0f) ? vibMul * _bendMul : _bendMul;
    if (mul != 1.0f) inc = (uint32_t)(inc * mul);

    for (int i = 0; i < TC_BLOCK; i++) {
      dst[i] += sineLookup(ph) * a;
      ph     += inc;
      a      += st;
    }
    v.phase[h] = ph;
    v.amp[h]   = (a < 0.0f) ? 0.0f : a;
  }

  // ---- noise layer (breath / bow / string-strike noise) -------------------
#ifdef TC_DBG_NO_BB
  if (false) {                       // debug: turn the broadband noise layer off completely
#else
  if (v.noise > 1e-6f || v.noiseStep > 0.0f) {
#endif
    float n  = v.noise;
    float st = v.noiseStep;
    uint32_t r = v.rng;
    const float nrm = v.noiseNrm;
    const float lb0 = v.nbLpB[0], lb1 = v.nbLpB[1], lb2 = v.nbLpB[2];
    const float la0 = v.nbLpA[0], la1 = v.nbLpA[1];
    const float hb0 = v.nbHpB[0], hb1 = v.nbHpB[1], hb2 = v.nbHpB[2];
    const float ha0 = v.nbHpA[0], ha1 = v.nbHpA[1];
    float lz[TC_NOISE_LP_STAGES][2];
    for (int k = 0; k < TC_NOISE_LP_STAGES; k++) { lz[k][0] = v.nbLpZ[k][0]; lz[k][1] = v.nbLpZ[k][1]; }
    float hz0 = v.nbHpZ[0], hz1 = v.nbHpZ[1];
    for (int i = 0; i < TC_BLOCK; i++) {
      r = r * 1664525u + 1013904223u;
      float x = ((int32_t)(r >> 8) * (1.0f / 8388608.0f)) - 1.0f;   // -1..1
      // Transposed direct form II: only two state variables and fixed
      // coefficients, so the compiler can unroll it
      for (int k = 0; k < TC_NOISE_LP_STAGES; k++) {
        const float y = lb0 * x + lz[k][0];
        lz[k][0] = lb1 * x - la0 * y + lz[k][1];
        lz[k][1] = lb2 * x - la1 * y;
        x = y;
      }
      {
        const float y = hb0 * x + hz0;
        hz0 = hb1 * x - ha0 * y + hz1;
        hz1 = hb2 * x - ha1 * y;
        x = y;
      }
      dst[i] += x * n * nrm * TC_NOISE_BB_GAIN;
      n  += st;
    }
    v.rng = r;
    for (int k = 0; k < TC_NOISE_LP_STAGES; k++) { v.nbLpZ[k][0] = lz[k][0]; v.nbLpZ[k][1] = lz[k][1]; }
    v.nbHpZ[0] = hz0; v.nbHpZ[1] = hz1;
    v.noise   = (n < 0.0f) ? 0.0f : n;
  }
}

// ---------------------------------------------------------------------------
void AudioSynthAdditive::update(void) {
  audio_block_t *bl = allocate();
  if (!bl) return;
  audio_block_t *br = allocate();
  if (!br) { release(bl); return; }

  for (int i = 0; i < TC_BLOCK; i++) { sAccL[i] = 0.0f; sAccR[i] = 0.0f; }

  const bool haveModel = (_model && _model->ready());

  // ---- global partial budget ----------------------------------------------
  // With release tails piling up, the total harmonic count can reach 512, which
  // blows the audio interrupt's time budget outright.
  // Past that, scale everything down proportionally: slightly duller, but no
  // dropouts.
  int wanted = 0;
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE) wanted += _v[i].nPart;

  float partScale = 1.0f;
  if (wanted > TC_PARTIAL_BUDGET) partScale = (float)TC_PARTIAL_BUDGET / (float)wanted;

  for (int i = 0; i < TC_N_VOICES; i++) {
    Voice &v = _v[i];
    if (v.stage == IDLE || !haveModel || !v.prof) continue;

    // ---- 1) envelope: read the measured profile curve ---------------------
    if (v.stage == PLAYING) {
      float tn = tc_timeWarp(v.tSec, v.refDur);
      if (tn > v.holdNorm) tn = v.holdNorm;      // sustaining instruments should not act out a "bow lift"

      if (tn >= 1.0f) {
        // The measured curve has run out. Decaying instruments have to keep
        // falling at the measured decay rate; they must not stop at the loud[31]
        // level — a piano does not plateau at −24 dB.
        //
        // It continues from the curve's last bin, so the first pass through here
        // has no step.
        if (v.envTail <= 0.0f) v.envTail = v.prof->loud[TC_N_KEYFRAME - 1];
        v.envTail *= v.tailCoef;
        v.env = v.envTail;
        // once it falls below audibility, kill it rather than let it idle and hold a voice
        if (v.env < 0.0006f) { v.env = 0.0f; v.stage = IDLE; continue; }
      } else {
        float pos = tc_clampf(tn, 0.0f, 1.0f) * (TC_N_KEYFRAME - 1);
        int   k   = (int)pos;
        if (k > TC_N_KEYFRAME - 2) k = TC_N_KEYFRAME - 2;
        float fr  = pos - k;
        v.env = v.prof->loud[k] * (1.0f - fr) + v.prof->loud[k + 1] * fr;
        if (v.env < 0.0f) v.env = 0.0f;
      }
    } else {                                     // RELEASE
      v.env *= v.rCoef;
      if (v.env < 0.0006f) {
        v.env = 0.0f;
        v.stage = IDLE;
        for (int h = 0; h < TC_N_PARTIAL; h++) { v.amp[h] = 0.0f; v.ampStep[h] = 0.0f; }
        v.noise = 0.0f; v.noiseStep = 0.0f;
        continue;
      }
    }
    v.tSec += TC_BLOCK_SEC;

    // ---- 2) ask the model what the harmonics look like right now ---------
    // When the budget is short, cut this voice's harmonic count; the ones cut have
    // to fade out, they must not be chopped off
    int nUse = (partScale < 1.0f) ? (int)(v.nPart * partScale) : v.nPart;
    if (nUse < 8) nUse = 8;
    if (nUse > v.nPart) nUse = v.nPart;
    for (int h = nUse; h < v.nPart; h++) v.ampStep[h] = -v.amp[h] * (1.0f / TC_BLOCK);

    float target[TC_N_PARTIAL], targetNoise = 0.0f;
    const float loud = v.env * v.vel;
    _model->harmonics(v.prof, v.f0, loud, tc_timeWarp(v.tSec, v.refDur),
                      v.stage == RELEASE, target, &targetNoise, nUse);

    // ---- 2a) asynchronous attack: each harmonic enters at its own time ---
    // On a real instrument the high harmonics arrive tens of milliseconds late;
    // starting them all together sounds very "electronic".
    // Active during the attack only; after that the gate is always 1, so the
    // sustain is untouched.
    if (v.tSec < 0.35f) {
      for (int h = 0; h < nUse; h++) {
        float t0 = v.onsetT[h];
        if (t0 <= 0.0f) continue;
        // 8 ms fade-in from t0, so a hard switch doesn't click
        float g = (v.tSec - t0) * 125.0f;
        target[h] *= tc_clampf(g, 0.0f, 1.0f);
      }
    }

    // ---- 2b) shimmer: an independent slow wobble per harmonic ------------
    // Measurements show about 8% of micro-fluctuation per harmonic on real
    // instruments; with none at all, long notes sound like a pipe organ.
    if (v.shimDepth > 0.001f) {
      for (int h = 0; h < nUse; h++) {
        v.shimPhase[h] += v.shimInc[h];
        if (v.shimPhase[h] > 6.2831853f) v.shimPhase[h] -= 6.2831853f;
        target[h] *= 1.0f + v.shimDepth * sinf(v.shimPhase[h]);
      }
    }

    // ---- 2b') harmonic jitter: the main source of aperiodic energy -------
    //
    // Measured spectral distribution of the residual (x[n]-x[n-T]) of a real
    // flute A4:
    //     0-0.9k 29.5%   0.9-2k 48.5%   2-5k 17.8%   above 5k 4.2%
    // 78% of the aperiodic energy hugs f0/h2/h3 — it is not broadband breath
    // noise at all, but sidebands produced by each harmonic wobbling slightly on
    // its own. Rendered as one broadband hiss layer, the synth side ended up with
    // 36% above 5k: entirely the wrong place, and it sounds like "hiss has been
    // added" rather than "this instrument breathes".
    //
    // Instead, apply a random amplitude modulation to each harmonic directly:
    // a_h -> a_h * (1 + sigma * g), where g is a unit-variance random sequence.
    // Sideband energy is then automatically proportional to that harmonic's
    // amplitude, the distribution matches the real one exactly, and the total
    // aperiodic ratio works out to exactly sigma^2 — the very number the analyzer
    // measures, so the two definitions line up perfectly.
    //
    // Drawn once per block with a modulation bandwidth of 172 Hz, so the sidebands
    // stay right next to the harmonics.
    float sigNow = v.jitterSigma;
#ifdef TC_DBG_NO_ATKJIT
    if (false) {                     // debug: disable attack jitter only, keep the attack broadband layer
#else
    if (v.atkJitSigma > 0.001f && v.tSec < 0.15f) {
#endif
      float e = expf(-v.tSec / 0.03f);
      sigNow = sqrtf(sigNow * sigNow + v.atkJitSigma * v.atkJitSigma * e * e);
    }
#ifdef TC_DBG_NO_JIT
    sigNow = 0.0f;                   // debug: turn harmonic jitter off completely
#endif
    if (sigNow > 0.001f) {
      uint32_t r = v.rng;
      for (int h = 0; h < nUse; h++) {
        r = r * 1664525u + 1013904223u;
        float u = ((int32_t)(r >> 8) * (1.0f / 8388608.0f)) - 1.0f;   // -1..1
        v.jit[h] = u * 1.732f;                            // *sqrt(3) -> unit variance
        target[h] *= tc_clampf(1.0f + sigNow * v.jit[h], 0.0f, 2.5f);
      }
      v.rng = r;
    }

    // ---- 2c) attack noise burst (bow / breath / string-strike noise) ------
    if (v.noiseAtk > 0.0f && v.tSec < 0.15f)
      targetNoise += v.noiseAtk * loud * expf(-v.tSec / 0.03f);

    const float invBlk = 1.0f / (float)TC_BLOCK;
    for (int h = 0; h < nUse; h++) v.ampStep[h] = (target[h] - v.amp[h]) * invBlk;
    v.noiseStep = (targetNoise - v.noise) * invBlk;

    // ---- 3) generate samples and apply equal-power pan -------------------
    renderVoice(v, sVoiceBuf);
    float gl = cosf(v.pan * (float)M_PI_2);
    float gr = sinf(v.pan * (float)M_PI_2);
    for (int k = 0; k < TC_BLOCK; k++) {
      sAccL[k] += sVoiceBuf[k] * gl;
      sAccR[k] += sVoiceBuf[k] * gr;
    }
  }

  // ---- 4) master volume + soft clip + convert to int16 --------------------
  const float g = _gain * 32767.0f;
  for (int k = 0; k < TC_BLOCK; k++) {
    bl->data[k] = (int16_t)softClip(sAccL[k] * g);
    br->data[k] = (int16_t)softClip(sAccR[k] * g);
  }

  transmit(bl, 0);
  transmit(br, 1);
  release(bl);
  release(br);
}

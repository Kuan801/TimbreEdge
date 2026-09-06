#include "additive_synth.h"

static float  sSine[TC_SINE_TBL_SIZE + 1];
static bool   sSineReady = false;

static void buildSine() {
  if (sSineReady) return;
  for (int i = 0; i <= TC_SINE_TBL_SIZE; i++)
    sSine[i] = sinf(2.0f * (float)M_PI * i / TC_SINE_TBL_SIZE);
  sSineReady = true;
}

static inline float sineLookup(uint32_t phase) {
  uint32_t idx  = phase >> (32 - TC_SINE_TBL_BITS);
  float    frac = (float)(phase & ((1u << (32 - TC_SINE_TBL_BITS)) - 1))
                  * (1.0f / (float)(1u << (32 - TC_SINE_TBL_BITS)));
  float a = sSine[idx], b = sSine[idx + 1];
  return a + (b - a) * frac;
}

#define SC_THRESH 24575.0f
#define SC_RANGE  8192.0f
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

DMAMEM static float sVoiceBuf[TC_BLOCK];
DMAMEM static float sAccL[TC_BLOCK];
DMAMEM static float sAccR[TC_BLOCK];

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

int AudioSynthAdditive::allocVoice(float midi) {

  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE && fabsf(_v[i].midi - midi) < 0.01f) return i;

  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage == IDLE) return i;

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

AudioSynthAdditive::NoteResult
AudioSynthAdditive::noteOn(float midi, float vel, float pan) {
  if (!_model) return NOTE_NO_MODEL;

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

  v.envTail = 0.0f;
  v.age   = _ageCounter++;
  v.vibPhase = 0.0f;

  const float bs = TC_BLOCK_SEC;
  v.prof     = p;
  v.refDur   = fmaxf(p->noteDur, 0.2f);
  v.holdNorm = (p->envHoldNorm > 0.01f) ? p->envHoldNorm : 1.0f;
  v.rCoef    = expf(-bs / fmaxf(p->release * 0.4f, 0.02f));

  {
    const float perSec = p->sustainDecayPerSec;
    v.tailCoef = (perSec > 0.0f && perSec < 0.999f) ? powf(perSec, bs) : 1.0f;
  }

  v.nPart = tc_partialCount(v.f0);

  for (int h = 0; h < v.nPart; h++)
    v.baseInc[h] = hzToInc(_model->harmonicHz(p, v.f0, h));

#ifdef TC_NOISE_FIXED_BAND

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

  float fLo = tc_clampf(fminf(v.f0 * 5.0f, fHi * 0.5f), 200.0f, 4000.0f);
#endif
  v.noiseFLo = fLo;
  v.noiseFHi = fHi;

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
      if (i >= 256) acc += x * x;
    }
    v.noiseNrm = 1.0f / (sqrtf(acc / 768.0f) + 1e-6f);
  }

  {
    float extra = tc_clampf(p->attackNoise - p->noiseGain, 0.0f, 0.9f);
#ifdef TC_DBG_NO_ATK
    extra = 0.0f;
#endif
    float hi    = tc_clampf(p->attackHighFrac, 0.0f, 0.9f);
    v.noiseAtk  = sqrtf(extra * hi);
    v.atkJitVar = extra * (1.0f - hi);
  }

  {
    float nf = tc_clampf(p->noiseGain, 0.0f, 0.9f);

    const float blkNyq = 0.5f / TC_BLOCK_SEC;
    const float xb = 2.0f * (float)M_PI * blkNyq / v.f0;
    float meanSin2 = 0.5f - ((fabsf(xb) > 1e-4f) ? sinf(xb) / (2.0f * xb) : 0.5f);
    meanSin2 = tc_clampf(meanSin2, 0.05f, 0.5f);
    const float comp = 1.0f / (2.0f * meanSin2 * 0.83f);

    float hiFrac = tc_clampf(p->noiseHighFrac, 0.0f, 0.9f);
    v.jitFrac = 1.0f - hiFrac;
    v.jitterSigma = sqrtf(nf * v.jitFrac * comp * TC_JITTER_CAL);

    {
      const float sigMax2 = TC_JIT_SIGMA_MAX * TC_JIT_SIGMA_MAX;
      if (v.atkJitVar * comp > sigMax2) {
        const float keep  = sigMax2 / comp;
        const float spill = v.atkJitVar - keep;
        v.atkJitVar = keep;

        v.noiseAtk  = sqrtf(v.noiseAtk * v.noiseAtk + spill);
      }
      v.atkJitSigma = sqrtf(v.atkJitVar * comp);

      if (v.jitterSigma > TC_JIT_SIGMA_MAX) v.jitterSigma = TC_JIT_SIGMA_MAX;
    }
    for (int h = 0; h < TC_N_PARTIAL; h++) v.jit[h] = 0.0f;
  }

  v.vibCents = tc_clampf(p->vibratoCents, 0.0f, _vibMaxCents);

  v.vibHz    = (p->vibratoHz > 2.0f) ? p->vibratoHz : _vibHz;

  for (int h = 0; h < v.nPart; h++) {
    if (h < TC_N_HARM) v.onsetT[h] = p->harmOnset[h];
    else               v.onsetT[h] = p->harmOnset[TC_N_HARM - 1];
  }

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

  for (int h = 0; h < v.nPart; h++)
    v.phase[h] = (uint32_t)((h * 2654435761u) ^ (v.age * 40503u));

  if (v.env < 0.001f) {
    for (int h = 0; h < TC_N_PARTIAL; h++) { v.amp[h] = 0.0f; v.ampStep[h] = 0.0f; }
    v.noise = 0.0f; v.noiseStep = 0.0f;
  }
  return NOTE_OK;
}

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

void AudioSynthAdditive::renderVoice(Voice &v, float *dst) {
  for (int i = 0; i < TC_BLOCK; i++) dst[i] = 0.0f;

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

#ifdef TC_DBG_NO_BB
  if (false) {
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
      float x = ((int32_t)(r >> 8) * (1.0f / 8388608.0f)) - 1.0f;

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

void AudioSynthAdditive::update(void) {
  audio_block_t *bl = allocate();
  if (!bl) return;
  audio_block_t *br = allocate();
  if (!br) { release(bl); return; }

  for (int i = 0; i < TC_BLOCK; i++) { sAccL[i] = 0.0f; sAccR[i] = 0.0f; }

  const bool haveModel = (_model && _model->ready());

  int wanted = 0;
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE) wanted += _v[i].nPart;

  float partScale = 1.0f;
  if (wanted > TC_PARTIAL_BUDGET) partScale = (float)TC_PARTIAL_BUDGET / (float)wanted;

  for (int i = 0; i < TC_N_VOICES; i++) {
    Voice &v = _v[i];
    if (v.stage == IDLE || !haveModel || !v.prof) continue;

    if (v.stage == PLAYING) {
      float tn = tc_timeWarp(v.tSec, v.refDur);
      if (tn > v.holdNorm) tn = v.holdNorm;

      if (tn >= 1.0f) {

        if (v.envTail <= 0.0f) v.envTail = v.prof->loud[TC_N_KEYFRAME - 1];
        v.envTail *= v.tailCoef;
        v.env = v.envTail;

        if (v.env < 0.0006f) { v.env = 0.0f; v.stage = IDLE; continue; }
      } else {
        float pos = tc_clampf(tn, 0.0f, 1.0f) * (TC_N_KEYFRAME - 1);
        int   k   = (int)pos;
        if (k > TC_N_KEYFRAME - 2) k = TC_N_KEYFRAME - 2;
        float fr  = pos - k;
        v.env = v.prof->loud[k] * (1.0f - fr) + v.prof->loud[k + 1] * fr;
        if (v.env < 0.0f) v.env = 0.0f;
      }
    } else {
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

    int nUse = (partScale < 1.0f) ? (int)(v.nPart * partScale) : v.nPart;
    if (nUse < 8) nUse = 8;
    if (nUse > v.nPart) nUse = v.nPart;
    for (int h = nUse; h < v.nPart; h++) v.ampStep[h] = -v.amp[h] * (1.0f / TC_BLOCK);

    float target[TC_N_PARTIAL], targetNoise = 0.0f;
    const float loud = v.env * v.vel;
    _model->harmonics(v.prof, v.f0, loud, tc_timeWarp(v.tSec, v.refDur),
                      v.stage == RELEASE, target, &targetNoise, nUse);

    if (v.tSec < 0.35f) {
      for (int h = 0; h < nUse; h++) {
        float t0 = v.onsetT[h];
        if (t0 <= 0.0f) continue;

        float g = (v.tSec - t0) * 125.0f;
        target[h] *= tc_clampf(g, 0.0f, 1.0f);
      }
    }

    if (v.shimDepth > 0.001f) {
      for (int h = 0; h < nUse; h++) {
        v.shimPhase[h] += v.shimInc[h];
        if (v.shimPhase[h] > 6.2831853f) v.shimPhase[h] -= 6.2831853f;
        target[h] *= 1.0f + v.shimDepth * sinf(v.shimPhase[h]);
      }
    }

    float sigNow = v.jitterSigma;
#ifdef TC_DBG_NO_ATKJIT
    if (false) {
#else
    if (v.atkJitSigma > 0.001f && v.tSec < 0.15f) {
#endif
      float e = expf(-v.tSec / 0.03f);
      sigNow = sqrtf(sigNow * sigNow + v.atkJitSigma * v.atkJitSigma * e * e);
    }
#ifdef TC_DBG_NO_JIT
    sigNow = 0.0f;
#endif
    if (sigNow > 0.001f) {
      uint32_t r = v.rng;
      for (int h = 0; h < nUse; h++) {
        r = r * 1664525u + 1013904223u;
        float u = ((int32_t)(r >> 8) * (1.0f / 8388608.0f)) - 1.0f;
        v.jit[h] = u * 1.732f;
        target[h] *= tc_clampf(1.0f + sigNow * v.jit[h], 0.0f, 2.5f);
      }
      v.rng = r;
    }

    if (v.noiseAtk > 0.0f && v.tSec < 0.15f)
      targetNoise += v.noiseAtk * loud * expf(-v.tSec / 0.03f);

    const float invBlk = 1.0f / (float)TC_BLOCK;
    for (int h = 0; h < nUse; h++) v.ampStep[h] = (target[h] - v.amp[h]) * invBlk;
    v.noiseStep = (targetNoise - v.noise) * invBlk;

    renderVoice(v, sVoiceBuf);
    float gl = cosf(v.pan * (float)M_PI_2);
    float gr = sinf(v.pan * (float)M_PI_2);
    for (int k = 0; k < TC_BLOCK; k++) {
      sAccL[k] += sVoiceBuf[k] * gl;
      sAccR[k] += sVoiceBuf[k] * gr;
    }
  }

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

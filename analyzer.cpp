#include "analyzer.h"
#include "wav_io.h"
#include "trainer.h"
#include <SD.h>

// ============================================================================
//  Own radix-2 complex FFT (no CMSIS version quirks, 2048 points ≈ 0.2 ms)
// ============================================================================
#define NFFT   TC_FFT_SIZE
#define NBITS  11                                   // 2^11 = 2048
static_assert(NFFT == 2048, "NBITS 要跟著 TC_FFT_SIZE 改");

DMAMEM static float  gRe[NFFT];
DMAMEM static float  gIm[NFFT];
DMAMEM static float  gTwRe[NFFT / 2];
DMAMEM static float  gTwIm[NFFT / 2];
DMAMEM static float  gWin[NFFT];
DMAMEM static float  gMag[NFFT / 2];
DMAMEM static float  gAvgMag[NFFT / 2];
DMAMEM static float  gRms[TC_MAX_FRAMES];

// How many attacks the last analysis counted in the recording. 1 = a normal single note.
//
// Not put into InstrumentProfile: that struct gets written to SD, and adding a field
// means bumping TC_PROFILE_MAGIC, which invalidates every BANK.BIN / PROFILE.BIN the
// user already has. This is just a by-product of "the analysis we just ran", there is
// no need to persist it.
static int gOnsetCount = 1;
int analyzerLastOnsetCount() { return gOnsetCount; }

// Three numbers on recording quality. The analysis has to sweep the whole file anyway,
// so measuring them costs next to nothing, but without them there is no way to answer
// "was this take actually usable?" --
// and that is the one thing the person standing in front of the machine wants to know.
static float gPeakAbs    = 0.0f;    // Absolute peak over the whole file, 0..1
static float gClipRatio  = 0.0f;    // Fraction of clipped samples
static float gNoiseFloor = 0.0f;    // Mean RMS before the attack (relative to peak)
float analyzerLastPeak()       { return gPeakAbs;    }
float analyzerLastClipRatio()  { return gClipRatio;  }
float analyzerLastNoiseFloor() { return gNoiseFloor; }
DMAMEM static float  gYin[NFFT / 2];
DMAMEM static float  gHarmAcc[TC_N_KEYFRAME][TC_N_HARM];
DMAMEM static float  gLoudAcc[TC_N_KEYFRAME];
DMAMEM static float  gCnt[TC_N_KEYFRAME];
DMAMEM static float  gBuf[NFFT];

// For fine attack analysis: per-harmonic envelope after heterodyne demodulation (103 bins × 2.9 ms)
#define ATK_FRAMES  ((int)(TC_ATK_WINDOW_SEC * TC_SAMPLE_RATE / TC_ATK_HOP))
DMAMEM static float  gAtkEnv[TC_N_HARM][ATK_FRAMES];
DMAMEM static float  gAtkTot[ATK_FRAMES];      // "Total energy" on the same time axis

// Per-harmonic amplitude tracks over the sustain, used to measure shimmer. Tracking the first 12 is representative enough.
#define SHIM_HARM 12
#define SHIM_MAX  192
DMAMEM static float  gShimTrack[SHIM_HARM][SHIM_MAX];

static bool gTablesReady = false;

static void (*gProgressCb)(float) = nullptr;
void analyzerSetProgressCallback(void (*cb)(float)) { gProgressCb = cb; }

static void buildTables() {
  if (gTablesReady) return;
  for (int i = 0; i < NFFT / 2; i++) {
    float a  = -2.0f * (float)M_PI * i / NFFT;
    gTwRe[i] = cosf(a);
    gTwIm[i] = sinf(a);
  }
  for (int i = 0; i < NFFT; i++)                    // Hann
    gWin[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (NFFT - 1));
  gTablesReady = true;
}

static inline uint32_t bitrev(uint32_t x) {
  x = ((x & 0x55555555u) << 1)  | ((x >> 1)  & 0x55555555u);
  x = ((x & 0x33333333u) << 2)  | ((x >> 2)  & 0x33333333u);
  x = ((x & 0x0F0F0F0Fu) << 4)  | ((x >> 4)  & 0x0F0F0F0Fu);
  x = (x << 24) | ((x & 0xFF00u) << 8) | ((x >> 8) & 0xFF00u) | (x >> 24);
  return x >> (32 - NBITS);
}

// In-place FFT, gRe/gIm in, gRe/gIm out
static void fft() {
  for (uint32_t i = 0; i < NFFT; i++) {
    uint32_t j = bitrev(i);
    if (j > i) {
      float t = gRe[i]; gRe[i] = gRe[j]; gRe[j] = t;
      t = gIm[i];       gIm[i] = gIm[j]; gIm[j] = t;
    }
  }
  for (uint32_t len = 2; len <= NFFT; len <<= 1) {
    uint32_t half = len >> 1;
    uint32_t step = NFFT / len;
    for (uint32_t i = 0; i < NFFT; i += len) {
      uint32_t k = 0;
      for (uint32_t j = 0; j < half; j++, k += step) {
        float wr = gTwRe[k], wi = gTwIm[k];
        uint32_t a = i + j, b = a + half;
        float xr = gRe[b] * wr - gIm[b] * wi;
        float xi = gRe[b] * wi + gIm[b] * wr;
        gRe[b] = gRe[a] - xr;  gIm[b] = gIm[a] - xi;
        gRe[a] = gRe[a] + xr;  gIm[a] = gIm[a] + xi;
      }
    }
  }
}

// ============================================================================
//  YIN pitch detection (input: the first NFFT points of gBuf)
//  Returns Hz, 0 on failure
// ============================================================================
static float yinPitch(const float *x, float sr) {
  const int W      = NFFT / 2;                       // 1024
  const int tauMin = (int)(sr / TC_F0_MAX);          // ~29
  const int tauMax = (int)(sr / TC_F0_MIN);          // ~678
  if (tauMax >= W) return 0.0f;

  // 1) Difference function
  gYin[0] = 1.0f;
  for (int tau = 1; tau <= tauMax; tau++) {
    float s = 0.0f;
    for (int j = 0; j < W; j++) { float d = x[j] - x[j + tau]; s += d * d; }
    gYin[tau] = s;
  }
  // 2) Cumulative mean normalisation
  float run = 0.0f;
  for (int tau = 1; tau <= tauMax; tau++) {
    run += gYin[tau];
    gYin[tau] = (run > 1e-12f) ? gYin[tau] * tau / run : 1.0f;
  }
  // 3) Absolute threshold -> first local minimum below threshold
  int best = -1;
  for (int tau = tauMin; tau < tauMax; tau++) {
    if (gYin[tau] < TC_YIN_THRESH) {
      while (tau + 1 < tauMax && gYin[tau + 1] < gYin[tau]) tau++;
      best = tau;
      break;
    }
  }
  if (best < 0) {                                     // Fall back to the global minimum
    float m = 1e30f;
    for (int tau = tauMin; tau < tauMax; tau++)
      if (gYin[tau] < m) { m = gYin[tau]; best = tau; }
    if (best < 0 || m > 0.6f) return 0.0f;            // Too aperiodic; call it noise
  }
  // 4) Parabolic interpolation
  float betterTau = (float)best;
  if (best > 0 && best < tauMax - 1) {
    float a = gYin[best - 1], b = gYin[best], c = gYin[best + 1];
    float den = 2.0f * (2.0f * b - a - c);
    if (fabsf(den) > 1e-9f) betterTau = best + (c - a) / den;
  }
  return sr / betterTau;
}

// ============================================================================
//  Fine attack analysis: heterodyne demodulation
//
//  The FFT's time resolution is capped by the window length (2048 points = 46 ms),
//  which simply cannot show a difference like "the high harmonics come in 20 ms
//  late". Heterodyne instead: multiply the signal by exp(-j2πf_h t) and low-pass,
//  and you get that harmonic's complex envelope, with a time resolution limited
//  only by the low-pass time constant (~3 ms here).
//
//  Cost: 32 harmonics × 13230 samples × ~12 ops ≈ 5 M ops, about 20 ms on the M7.
// ============================================================================
//  Streamed with samples on the outside and harmonics on the inside, so the whole
//  attack never has to be held in memory -- only 4 state variables per harmonic
//  (phase + low-pass) are kept.
static void heterodyneAttack(WavReader &wav, uint32_t startSample,
                             float f0, float sr, int nHarm) {
  const float lpA = 1.0f - expf(-2.0f * (float)M_PI * TC_ATK_LP_HZ / sr);
  const int   nSamp = (int)(TC_ATK_WINDOW_SEC * sr);

  static float pr[TC_N_HARM], pi[TC_N_HARM];    // Rotating phase
  static float lr[TC_N_HARM], li[TC_N_HARM];    // Low-pass state
  static float cw[TC_N_HARM], sw[TC_N_HARM];    // Rotation per step
  bool live[TC_N_HARM];

  for (int h = 0; h < nHarm; h++) {
    float fh = f0 * (h + 1);
    live[h] = (fh <= sr * 0.48f);
    float w = -2.0f * (float)M_PI * fh / sr;
    cw[h] = cosf(w); sw[h] = sinf(w);
    pr[h] = 1.0f; pi[h] = 0.0f;
    lr[h] = 0.0f; li[h] = 0.0f;
    for (int i = 0; i < ATK_FRAMES; i++) gAtkEnv[h][i] = 0.0f;
  }

  float totLp = 0.0f;                           // Total energy (same low-pass, so the time axes line up)
  for (int i = 0; i < ATK_FRAMES; i++) gAtkTot[i] = 0.0f;

  const int CHUNK = 1024;
  int outIdx = 0, n = 0;
  while (n < nSamp && outIdx < ATK_FRAMES) {
    int want = (nSamp - n < CHUNK) ? (nSamp - n) : CHUNK;
    if (wav.readMono(startSample + n, gBuf, want) == 0) break;

    for (int k = 0; k < want && outIdx < ATK_FRAMES; k++, n++) {
      const float xv = gBuf[k];
      totLp += lpA * (xv * xv - totLp);
      for (int h = 0; h < nHarm; h++) {
        if (!live[h]) continue;
        lr[h] += lpA * (xv * pr[h] - lr[h]);
        li[h] += lpA * (xv * pi[h] - li[h]);
        float npr = pr[h] * cw[h] - pi[h] * sw[h];
        float npi = pr[h] * sw[h] + pi[h] * cw[h];
        pr[h] = npr; pi[h] = npi;
      }
      if ((n & 255) == 255) {                   // Renormalise periodically to avoid numerical drift
        for (int h = 0; h < nHarm; h++) {
          float m = 1.0f / sqrtf(pr[h] * pr[h] + pi[h] * pi[h] + 1e-20f);
          pr[h] *= m; pi[h] *= m;
        }
      }
      if ((n % TC_ATK_HOP) == 0) {
        for (int h = 0; h < nHarm; h++)
          gAtkEnv[h][outIdx] = sqrtf(lr[h] * lr[h] + li[h] * li[h]);
        gAtkTot[outIdx] = totLp;
        outIdx++;
      }
    }
  }
}

// ============================================================================
//  Inharmonic (noise) fraction: the periodic-residual method
//
//  A signal of period T satisfies x[n] ≈ x[n-T], so x[n] − x[n−T] cancels out the
//  periodic part and leaves the aperiodic one (bow noise, breath, string-strike
//  noise). For uncorrelated noise the subtraction doubles the power, hence
//  noise ratio = E(diff) / (2·E(x)).
//
//  Why not the previous "heterodyne residual": resolving a 2.9 ms transient needs
//  about 350 Hz of bandwidth, but separating harmonics spaced 220 Hz apart allows
//  only 110 Hz -- time-frequency uncertainty is just there. The result was that the
//  per-harmonic detection bands overlapped, the whole spectrum got counted as
//  harmonic, and the residual was permanently 0 (material with a true value of 35%
//  read as 0.0%). The periodic residual has no such contradiction: it works in the
//  time domain and its resolution is limited only by the window length.
//
//  Returns 0..1; T uses linear interpolation to support non-integer periods.
//
//  ★ The period length must be found here; the whole-note f0 will not do (TC-GUITAR)
//
//  On plucked instruments the pitch drifts after the pluck: on this batch of guitar
//  material the analyzer itself reported "pitch swing 48~57 cents, but no 3~9 Hz
//  periodicity" (so it is drift, not vibrato). Computing x[n]-x[n-T] with the f0
//  averaged over the whole note, a T that is off by 50 cents (about 3%) is already
//  enough to misalign the two periods, and what the subtraction leaves is "the
//  phases do not line up" rather than noise.
//
//  Measured consequence: the guitar's sustain noise ratio came out 2~13 times too
//  high (D3 measured 0.8%, while the same signal measured with evaluate.py's method
//  is only 0.06%). The synth faithfully emits broadband noise at that level, so
//  every note carries a layer of hiss no real guitar has -- noise level off by
//  +12 dB, and all four metrics (LSD, spectrogram, centroid) are blind to it (they
//  were never sensitive to anything at -70 dB in the first place).
//
//  Approach: scan the period length within ±3% and take whichever gives the smallest
//  residual. By definition that measures "the energy no nearby period can explain",
//  which is what the aperiodic component is supposed to mean. It barely affects
//  instruments that really do have broadband noise (flute breath, violin bow noise)
//  -- a different T cannot explain white noise away either, so the minimum equals
//  the original value.
static float periodicNoiseRatio(WavReader &wav, uint32_t startSample,
                                float f0, float sr, float windowSec) {
  const float Tf0 = sr / f0;
  const int   nWin = (int)(windowSec * sr);
  const int   need = nWin + (int)(Tf0 * 1.03f) + 2;
  if (need > NFFT) return 0.0f;                 // Give up if it will not fit in gBuf

  if (wav.readMono(startSample, gBuf, need) < (uint32_t)need) return 0.0f;

  float best = 1.0f;
  // 7 candidates over ±3%: the drift measured on the guitar is ±3%, going finer buys
  // less than 0.1 dB, and this runs 6 times per note on the Teensy
  // (1 attack + 5 sustain)
  for (int k = -3; k <= 3; k++) {
    const float Tf   = Tf0 * (1.0f + 0.01f * k);
    const int   Ti   = (int)Tf;
    const float frac = Tf - Ti;
    if (Ti < 2 || Ti + 1 + nWin > need) continue;

    // Compare period by period, and first bring the previous period's level into
    // line with the current one.
    //
    // Without that, the attack counts "the envelope is rising fast" entirely as
    // noise: a piano's amplitude can double within one period (3.8 ms), so most of
    // x[n]-x[n-T] comes from the level change rather than from noise.
    // An early version used that overestimated value as an "amplitude"; the number
    // happened to be small, so nothing broke. Once the correct energy->amplitude
    // conversion (square root) went in, the error blew up into a blob of broadband
    // noise on every note onset, and the piano's centroid correlation fell from
    // 0.968 to 0.841.
    double ex = 0.0, ed = 0.0;
    for (int base = Ti + 1; base < Ti + 1 + nWin; base += Ti) {
      int last = base + Ti;
      if (last > Ti + 1 + nWin) last = Ti + 1 + nWin;
      double ea = 0.0, eb = 0.0;
      for (int n = base; n < last; n++) {
        float prev = gBuf[n - Ti] * (1.0f - frac) + gBuf[n - Ti - 1] * frac;
        ea += (double)gBuf[n] * gBuf[n];
        eb += (double)prev * prev;
      }
      if (ea < 1e-15 || eb < 1e-15) continue;
      const float g = (float)sqrt(ea / eb);          // Bring the previous period's level into line with this one
      for (int n = base; n < last; n++) {
        float prev = (gBuf[n - Ti] * (1.0f - frac) + gBuf[n - Ti - 1] * frac) * g;
        float d = gBuf[n] - prev;
        ed += (double)d * d;
      }
      ex += ea;
    }
    if (ex < 1e-12) continue;
    const float r = tc_clampf((float)(ed / (2.0 * ex)), 0.0f, 1.0f);
    if (r < best) best = r;
  }
  return (best > 0.999f) ? 0.0f : best;
}

// How much of the residual (the aperiodic part) lands at high frequencies.
//
// Why it is needed: flute breath and violin bow noise are similar in "amount" but
// land in completely different places -- measured on real material, the share of the
// residual in 2~5 kHz is 17.8% for flute and 65.6% for violin.
// Synthesise both with the same spectral shape and the violin loses its scratchy bow.
// Returns "the share of total residual energy above 5*f0"; the synth uses it to
// decide how much should go through the broadband noise layer.
static float noiseHighFraction(WavReader &wav, uint32_t startSample,
                               float f0, float sr) {
  const int Ti = (int)(sr / f0 + 0.5f);
  const int nWin = NFFT - Ti - 2;
  if (nWin < 1024) return 0.5f;
  if (wav.readMono(startSample, gBuf, NFFT) < (uint32_t)NFFT) return 0.5f;

  // Zero-pad the residual to NFFT and run the existing fixed-length FFT (gRe/gIm in, gRe/gIm out)
  for (int i = 0; i < nWin; i++) {
    float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (nWin - 1)));
    gRe[i] = (gBuf[Ti + 1 + i] - gBuf[i + 1]) * w;
    gIm[i] = 0.0f;
  }
  for (int i = nWin; i < (int)NFFT; i++) { gRe[i] = 0.0f; gIm[i] = 0.0f; }
  fft();

  const float binHz  = sr / (float)NFFT;
  const int   kSplit = (int)(5.0f * f0 / binHz + 0.5f);
  double lo = 0.0, hi = 0.0;
  for (int k = 1; k < (int)NFFT / 2; k++) {
    double e = (double)gRe[k] * gRe[k] + (double)gIm[k] * gIm[k];
    if (k < kSplit) lo += e; else hi += e;
  }
  double tot = lo + hi;
  if (tot < 1e-15) return 0.5f;
  return tc_clampf((float)(hi / tot), 0.0f, 1.0f);
}

// Returns the time (seconds) at which this harmonic reaches 50% of its own peak.
// Returns -1 if it cannot be found.
//
// It requires 3 consecutive bins above the threshold: the broadband spike at the
// instant of attack (bow noise / string-strike noise) briefly excites every
// harmonic's demodulator at once, and if only the first threshold crossing counted,
// every harmonic would be judged t=0 and the asynchrony would be unmeasurable.
static float onsetTimeOf(int h) {
  float mx = 0.0f;
  for (int i = 0; i < ATK_FRAMES; i++) if (gAtkEnv[h][i] > mx) mx = gAtkEnv[h][i];
  if (mx < 1e-7f) return -1.0f;
  const float th = 0.5f * mx;
  int run = 0;
  for (int i = 0; i < ATK_FRAMES; i++) {
    if (gAtkEnv[h][i] >= th) {
      if (++run >= 3) return (float)(i - 2) * TC_ATK_HOP / TC_SAMPLE_RATE;
    } else {
      run = 0;
    }
  }
  return -1.0f;
}

// ============================================================================
static int cmpf(const void *a, const void *b) {
  float d = *(const float *)a - *(const float *)b;
  return (d > 0) - (d < 0);
}

// Find the peak near targetBin and interpolate parabolically; returns the amplitude
// and hands the exact position back in outBin.
// rad = how many bins to search each way. The default of 2 is for the case where
// "the theoretical position is roughly the actual position"; on strings with
// inharmonicity the high harmonics wander several bins off, and too narrow a window
// stops at the window edge, truncating the measured shift -- which is exactly why the
// old version got a "conveniently" plausible B for the piano (two errors cancelling).
// The caller has to work out rad itself; the limit must be less than half the spacing
// between adjacent harmonics, or it will grab the neighbour.
static float peakAt(float targetBin, int nBins, float *outBin, int rad = 2) {
  if (rad < 1) rad = 1;
  int c = (int)(targetBin + 0.5f);
  if (c < rad + 1) c = rad + 1;
  if (c > nBins - rad - 2) c = nBins - rad - 2;

  int best = c;
  for (int i = c - rad; i <= c + rad; i++) if (gMag[i] > gMag[best]) best = i;

  float a = gMag[best - 1], b = gMag[best], cc = gMag[best + 1];
  float den = a - 2.0f * b + cc;
  float d   = (fabsf(den) > 1e-12f) ? 0.5f * (a - cc) / den : 0.0f;
  if (d > 1.0f)  d = 1.0f;
  if (d < -1.0f) d = -1.0f;

  if (outBin) *outBin = best + d;
  return b - 0.25f * (a - cc) * d;                    // Interpolated peak
}

// ============================================================================
bool analyzeWavFile(const char *wavPath, InstrumentProfile &out,
                    const char *csvDumpPath, TrainSet *trainSet) {
  buildTables();
  memset(&out, 0, sizeof(out));

  WavReader wav;
  if (!wav.open(wavPath)) return false;

  const float sr    = (float)wav.sampleRate();
  const uint32_t N  = wav.frames();
  int nFrames = (int)((N > NFFT) ? ((N - NFFT) / TC_HOP + 1) : 1);
  if (nFrames > TC_MAX_FRAMES) nFrames = TC_MAX_FRAMES;

  uint32_t t0 = millis();
  Serial.printf("[ANA] 分析 %s  %d 格\n", wavPath, nFrames);

  // ------------------------------------------------------- 1) RMS envelope --
  float rmsMax = 0.0f;
  gPeakAbs = 0.0f; gClipRatio = 0.0f; gNoiseFloor = 0.0f;
  uint32_t clipped = 0, counted = 0;
  for (int f = 0; f < nFrames; f++) {
    wav.readMono((uint32_t)f * TC_HOP, gBuf, TC_HOP);
    float s = 0.0f;
    for (int i = 0; i < TC_HOP; i++) {
      s += gBuf[i] * gBuf[i];
      const float a = fabsf(gBuf[i]);
      if (a > gPeakAbs) gPeakAbs = a;
      // 0.997 rather than 1.0: int16 full scale is 32767, and clipping usually happens
      // on the analog side, so by the time it reaches the ADC it is already a flat top
      // and the highest point need not land exactly on 32767.
      if (a >= 0.997f) clipped++;
    }
    counted += TC_HOP;
    gRms[f] = sqrtf(s / TC_HOP);
    if (gRms[f] > rmsMax) rmsMax = gRms[f];
  }
  gClipRatio = counted ? (float)clipped / (float)counted : 0.0f;
  if (rmsMax < 1e-4f) {
    Serial.println(F("[ANA] 訊號幾乎是靜音，放棄"));
    wav.close();
    return false;
  }
  for (int f = 0; f < nFrames; f++) gRms[f] /= rmsMax;   // Normalise to 0..1

  // onset / peak / offset
  int onset = 0;
  while (onset < nFrames && gRms[onset] < 0.08f) onset++;
  int offset = nFrames - 1;
  while (offset > onset && gRms[offset] < 0.04f) offset--;
  if (offset - onset < 4) { Serial.println(F("[ANA] 有效音長太短")); wav.close(); return false; }

  int peakIdx = onset;
  for (int f = onset; f <= offset; f++) if (gRms[f] > gRms[peakIdx]) peakIdx = f;

  // Whatever comes before the attack is this take's noise floor. Measured on the take
  // recorded by holding a phone speaker up to the microphone, the trigger fired a full
  // 1.7 seconds early -- what triggered it was 30~190 Hz room rumble at an SNR of only
  // 7 dB, and the note itself occupied just the last 0.3 s. This number shows that;
  // peak does not.
  if (onset > 0) {
    float s = 0.0f;
    for (int f = 0; f < onset; f++) s += gRms[f];
    gNoiseFloor = s / onset;                       // gRms is already normalised to 0..1
  }

  const float frameSec = TC_HOP / sr;
  out.noteDur = (offset - onset) * frameSec;

  // ---------------------------------------------------------------------------
  //  Is there more than one note in this recording?
  //
  //  Every measurement below is built on "one sustained single note": ADSR, decay
  //  rate, shimmer, sustain noise ratio -- all of them assume a single attack.
  //  Record a run of repeated plucks and the whole thing collapses -- silently, still
  //  producing a profile that looks perfectly normal.
  //
  //  Measured (guitar, a 2-second mic take, one pluck every 0.30 s, 6~7 of them):
  //      decay 0.950 (true ~0.5)      each pluck resets the level; the fit sees no decay
  //      shimmer 20% (true 0%)        repeated attacks read as huge amplitude swing, hits cap
  //      attack 131~264 ms (true ~30) attack detection disturbed by the later plucks
  //      noise 0.258 (true 0.012)     sustain window straddles several attack transients
  //  The synthesised guitar therefore drones on like an organ, and wobbles badly.
  //
  //  Fast-decaying instruments like the guitar are the easiest to catch out this way:
  //  when you have 2 seconds to fill, you naturally just keep plucking.
  //
  //  Criterion: the level jumps by more than 6 dB within one bin, it ends up above 15%
  //  of peak, and there are at least 150 ms between two of them. The thresholds come
  //  from measured data -- genuinely single-note material (the source files, piano,
  //  violin, flute, trumpet) all count exactly 1.
  // ---------------------------------------------------------------------------
  //  The criterion cannot be "the level jumped" alone -- the attack's own ramp is a
  //  jump, and 35 of 68 known single-note files got misjudged. A real re-pluck
  //  **drops first and then bounces back**, so we require a recovery of at least 3x
  //  measured from the lowest point since the previous attack.
  //
  //  And only after the main peak: before it, that is the attack, so there cannot be
  //  a second one.
  {
    const int minGap = (int)(0.15f / frameSec + 0.5f);
    int   count = 0, last = -9999;
    float runMin = 1e9f;                      // Lowest point since the previous attack
    for (int f = peakIdx + 1; f <= offset; f++) {
      if (gRms[f] < runMin) runMin = gRms[f];
      if (f - last < minGap) continue;
      if (gRms[f] < 0.25f)  continue;         // Swings that are too quiet do not count (gRms is already normalised)
      if (runMin > 1e-4f && gRms[f] / runMin >= 3.0f) {
        count++; last = f; runMin = gRms[f];
      }
    }
    // The main peak itself counts as one
    gOnsetCount = count + 1;

    if (gOnsetCount >= 2) {
      Serial.println();
      Serial.printf("[ANA] 警告：這段錄音裡偵測到 %d 次起音，不是單一個音。\n", gOnsetCount);
      Serial.println(F("       這套分析假設「撥/彈一次，讓它自己響完」，多次起音會讓"));
      Serial.println(F("       衰減速率、起音時間、shimmer、噪聲比全部量錯，"));
      Serial.println(F("       合成出來會變成一直響而且抖動很大的音色。"));
      Serial.println(F("       請重錄：只撥一次就停手，讓它自然衰減到底。"));
      Serial.println(F("       （衰減快的樂器例如吉他最容易不小心多撥幾下）"));
    }
  }

  // ----------------------------------------------------------- 2) ADSR fit --
  out.attack = fmaxf((peakIdx - onset) * frameSec, 0.003f);

  // ---- The "body": where the envelope stays above 60% of peak --------------
  //
  // The old version defined the sustain window as "35%~75% after the peak", which goes
  // badly wrong on real material: on a 2.6 s violin that window landed exactly where
  // the bow was finishing and the sound was dying away (envelope 0.61 -> 0.12), so the
  // violin was classified as a "decaying instrument" and got exactly the same envelope
  // behaviour as a piano -- flattening away the single biggest audible difference
  // between the two.
  //
  // Replaced by a structural criterion: the body's share of the note length.
  //   piano  body 0.19 s / note 3.25 s =  6%   -> decaying
  //   violin body 1.20 s / note 1.70 s = 71%   -> sustaining
  // This measurement has nothing to do with "decay rate", so it cannot be misled by
  // the length of the material or by how the bow stroke ends.
  int bodyA = onset, bodyB = offset;
  {
    int f = onset;
    while (f <= offset && gRms[f] < 0.6f) f++;
    if (f <= offset) bodyA = f;
    f = offset;
    while (f > bodyA && gRms[f] < 0.6f) f--;
    bodyB = f;
  }
  // A body too short to measure means the decay is extremely fast (some piano notes are
  // exactly like that: >60% only lasts two bins). Those have to be classified as
  // "decaying", and only a short stretch around the peak is taken as the spectral
  // measurement window.
  //
  // An early version had bodyB = (peakIdx + offset)/2 as the fallback here -- exactly
  // backwards, handing back an absurdly long fake body that classified piano Bb4 as
  // sustaining (body 49%), flattening the envelope into a long note that never decays.
  const bool bodyTooShort = (bodyB <= bodyA + 1);
  if (bodyTooShort) {
    bodyA = peakIdx;
    int span = (offset - peakIdx) / 8;
    bodyB = peakIdx + (span < 3 ? 3 : span);
    if (bodyB > offset) bodyB = offset;
  }

  const float bodyFrac = bodyTooShort
                         ? 0.0f
                         : (float)(bodyB - bodyA) / (float)(offset - onset + 1);

  // The peak position matters too: a plucked or struck string gets all its energy in
  // the attack and can only get quieter afterwards; physically it cannot "grow louder
  // as it goes". So a peak in the second half rules out decaying.
  //
  // This one is crucial on real material: violins are often crescendos (all 12 measured
  // files are, the envelope climbing from 0.3 to 1.0), which makes the body both late
  // and short, so bodyFrac alone would misjudge bowed strings as plucked -- measured,
  // G4's body is only 15%.
  const float peakPos = (float)(peakIdx - onset) / (float)(offset - onset + 1);
  const bool  decaying = (bodyFrac < 0.25f) && (peakPos < 0.25f);

  // Spectral envelope averaging, shimmer and the noise measurements all switch to the body interval
  int susA = bodyA, susB = bodyB;

  // ---- shimmer's own sampling window ----
  //
  // shimmer should not be tied to the "body". The comment above says so itself: violins
  // are almost always crescendos, which makes the body late and short. The high notes
  // in this batch are worse still -- measured, Ab5's body is only 18% of the note
  // (0.92~1.13 s, 19 bins) and B5's is 21% (23 bins), while shimmer refuses to compute
  // below 24 bins, so both of those notes came out with shimmer 0. B5 missed by one bin.
  //
  // And what shimmer measures is "the jitter left after dividing by a 9-bin (104 ms)
  // local moving average", and that detrending already removes crescendos, diminuendos
  // and any shape slower than the window. So confining it to the body does nothing for
  // the measurement, it just throws away usable bins.
  //
  // Only relaxed for sustaining instruments. Decaying ones (piano/plucked) stay as they
  // were: their body detection is right, and pulling the window earlier would swallow
  // the very fast decay right after the attack -- shimmer was rewritten to use a
  // log-domain moving average precisely to fix the piano being measured at a fake
  // 22.6%, and we are not going back.
  // Only the start is extended; the end always stays at the end of the body.
  //
  // Moving the end out to offset was tried: shimN went from 19 to 89, but accN collapsed
  // instead (B3 fell from 9 to 1, breaking notes that had measured fine). The reason is
  // the "drop harmonics that decay too fast" gate below, which compares the amplitude at
  // the end of the window against the one at the start; once the bow release is inside
  // the window, last/first is almost certain to be below 0.25, so every harmonic gets
  // judged as decaying and all of them are thrown out.
  // The end of the body is by definition where the release begins; that judgement is
  // correct, do not touch it.
  int shimA = susA, shimB = susB;
  if (!decaying) {
    const int a = onset + (int)(TC_SHIM_START_SEC / frameSec);  // Skip the attack transient (faster than the MA window, so it cannot be removed)
    if (a < shimA) shimA = a;
  }
  {
    static float tmp[TC_MAX_FRAMES];
    int n = 0;
    for (int f = susA; f <= susB && n < TC_MAX_FRAMES; f++) tmp[n++] = gRms[f];
    qsort(tmp, n, sizeof(float), cmpf);
    out.sustain = tc_clampf(tmp[n / 2], 0.02f, 1.0f);
  }
  // decay = time from the start of the body to falling to sustain*1.05 (only used by sustaining)
  {
    int f = bodyA;
    while (f < bodyB && gRms[f] > out.sustain * 1.05f) f++;
    out.decay = fmaxf((f - bodyA) * frameSec, 0.01f);
  }

  // release:
  //   sustaining -> the stretch after the body where the sound dies away (bow/breath stops)
  //   decaying   -> that is the damper time, which is short; the long natural decay is
  //                 sustainDecayPerSec's job and must not be counted into release, or the
  //                 piano's release comes out as several seconds
  out.release = decaying ? 0.20f
                         : fmaxf((offset - bodyB) * frameSec, 0.10f);

  // Natural decay over the sustain: log-domain linear regression, far more robust than taking the two endpoints
  {
    int rA = bodyA;
    int rB = decaying ? offset : bodyB;      // Decaying notes need a longer stretch before the real decay rate can be measured
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    int   n = 0;
    for (int f = rA; f <= rB; f++) {
      if (gRms[f] < 1e-4f) continue;
      float y = logf(gRms[f]);
      sx += f; sy += y; sxx += (float)f * f; sxy += f * y; n++;
    }
    float perSec = 1.0f;
    if (n >= 8) {
      float den = n * sxx - sx * sx;
      if (fabsf(den) > 1e-6f) {
        float slopePerFrame = (n * sxy - sx * sy) / den;
        perSec = expf(slopePerFrame / frameSec);
      }
    }
    // The lower bound used to be 0.15. Measured, the true values for plucked high notes
    // sit at 0.130~0.179 (measured independently by regressing the envelope), i.e. the
    // bound was active the whole time, squashing different notes down to an identical
    // 0.150 -- misleading when displayed, and one fewer piece of useful information in
    // the "change instrument" timbre distance.
    //
    // Relaxed to 0.05 (down to 5% per second, about -26 dB/s). Any lower stops sounding
    // like an instrument and starts sounding like a noise burst.
    //
    // Note: this value does not affect synthesis. Playback uses the measured envelope
    // curve loud[32] (see the notes in profile.h); this is only for display and for
    // profileTimbreDistance.
    if (decaying) perSec = tc_clampf(perSec, 0.05f, 0.97f);
    else          perSec = tc_clampf(perSec, 0.95f, 1.0f);
    out.sustainDecayPerSec = (perSec > 0.97f) ? 1.0f : perSec;
  }

  out.envHoldNorm = 1.0f;   // The sustaining curve gets flattened later on, so there is no need to stop early

  Serial.printf("[ANA] 本體 %.2f~%.2f s（佔音長 %.0f%%）-> 判定為%s\n",
                (bodyA - onset) * frameSec, (bodyB - onset) * frameSec,
                bodyFrac * 100.0f, decaying ? "衰減型" : "持續型");

  // ------------------------------------------------- 3) Median f0 from YIN --
  float cand[9];
  int   nc = 0;
  for (int k = 0; k < 9 && nc < 9; k++) {
    int f = peakIdx + (int)((offset - peakIdx) * (k / 9.0f));
    if (f < 0 || f > offset) continue;
    uint32_t pos = (uint32_t)f * TC_HOP;
    if (pos + NFFT > N) break;
    wav.readMono(pos, gBuf, NFFT);
    float p = yinPitch(gBuf, sr);
    if (p > TC_F0_MIN && p < TC_F0_MAX) cand[nc++] = p;
  }
  if (nc == 0) { Serial.println(F("[ANA] 抓不到基頻（訊號太雜或不是單音）")); wav.close(); return false; }
  qsort(cand, nc, sizeof(float), cmpf);
  out.f0 = cand[nc / 2];
  Serial.printf("[ANA] f0 = %.2f Hz  (%d 個候選)\n", out.f0, nc);

  // ----------------------------------------------- 3a) Vibrato depth --------
  //
  // The old version used "the spread of the 9 YIN candidates" as vibrato depth. That is
  // the wrong quantity: the spread also picks up the player's slow pitch drift, YIN's
  // own estimation error, and the occasional octave error. Flute material labelled
  // nonvib (no vibrato) measured 2.3~11.5 cents of vibrato in the old version, so
  // synthesis added a wobble the real material never had.
  //
  // Physically vibrato is "periodic pitch modulation at 4~8 Hz", so what has to be
  // measured is periodicity, not spread:
  //   1) Heterodyne at f0 to pull out the instantaneous frequency track (172 Hz sample
  //      rate, up to 1.5 seconds)
  //   2) Subtract a 0.5 s moving average -- takes out the slow drift, leaves the wobble
  //   3) Sweep a DFT over 3~9 Hz for the peak; only when that band accounts for a large
  //      enough share of the residual variance is it accepted as real vibrato,
  //      otherwise report 0 (it was only estimation noise)
  {
    // The demodulator's accumulation window must be an integer number of f0 periods.
    // A rectangular window's frequency response is zero at integer multiples of
    // sr/DECIM, so with the window aligned to the f0 period every harmonic's offset
    // (n*f0) lands exactly on a zero -- a perfect comb of notches.
    // Without that alignment the second harmonic leaks into baseband -- flute A4 has an
    // h2 stronger than the fundamental, and with DECIM=256 the leakage is -18 dB, which
    // puts the measured pitch-swing noise floor at 35 cents, enough to bury even an
    // injected 30-cent vibrato (confirmed with a positive control).
    int m = (int)(out.f0 / 130.0f + 0.5f);        // Puts the demodulated sample rate somewhere around 130 Hz
    if (m < 1) m = 1;
    const int   DECIM = (int)(m * sr / out.f0 + 0.5f);   // Cannot be called DEC: Print.h has #define DEC 10
    const int   MAXP = 256;
    const float fsD   = sr / (float)DECIM;
    static float trk[MAXP];
    int np = 0;

    const float w  = 2.0f * (float)M_PI * out.f0 / sr;
    const float cw = cosf(w), sw = sinf(w);
    float cr = 1.0f, ci = 0.0f;                   // Recursive oscillator; faster than calling sinf per point
    float accI = 0.0f, accQ = 0.0f;
    int   cnt = 0;
    float pI = 0.0f, pQ = 0.0f;
    bool  havePrev = false;

    uint32_t pos = (uint32_t)bodyA * TC_HOP;
    const uint32_t endPos = (uint32_t)bodyB * TC_HOP;
    while (pos + NFFT <= N && pos < endPos && np < MAXP) {
      wav.readMono(pos, gBuf, NFFT);
      for (int i = 0; i < NFFT && np < MAXP; i++) {
        accI += gBuf[i] * cr;
        accQ -= gBuf[i] * ci;
        // Rotate + renormalise periodically (float recursion slowly drifts off the unit circle)
        float nr = cr * cw - ci * sw;
        ci = cr * sw + ci * cw;
        cr = nr;
        if (((i & 1023) == 1023)) {
          float g = 1.5f - 0.5f * (cr * cr + ci * ci);
          cr *= g; ci *= g;
        }
        if (++cnt >= DECIM) {
          if (havePrev) {
            // The argument of z[k] * conj(z[k-1]) = the mean phase increment over that stretch
            float re = accI * pI + accQ * pQ;
            float im = accQ * pI - accI * pQ;
            float dphi = atan2f(im, re);
            trk[np++] = 1731.2f * (dphi * fsD / 6.2831853f) / out.f0;   // Cents
          }
          pI = accI; pQ = accQ; havePrev = true;
          accI = accQ = 0.0f; cnt = 0;
        }
      }
      pos += NFFT;
    }

    out.vibratoCents = 0.0f;
    out.vibratoHz    = 0.0f;
    if (np >= 48) {
      // 2) Subtract a 0.5 s moving average to take out the slow drift
      const int HW = (int)(0.25f * fsD);
      static float det[MAXP];
      for (int i = 0; i < np; i++) {
        int a = i - HW, b = i + HW;
        if (a < 0) a = 0;
        if (b > np - 1) b = np - 1;
        float m = 0.0f;
        for (int j = a; j <= b; j++) m += trk[j];
        det[i] = trk[i] - m / (float)(b - a + 1);
      }
      float tot = 0.0f;
      for (int i = 0; i < np; i++) tot += det[i] * det[i];
      tot /= (float)np;

      // 3) Sweep a DFT over 3~9 Hz
      float best = 0.0f, bestHz = 0.0f;
      for (float fv = 3.0f; fv <= 9.01f; fv += 0.25f) {
        float re = 0.0f, im = 0.0f;
        float ph = 0.0f, dp = 2.0f * (float)M_PI * fv / fsD;
        for (int i = 0; i < np; i++) { re += det[i] * cosf(ph); im -= det[i] * sinf(ph); ph += dp; }
        float mag = 2.0f * sqrtf(re * re + im * im) / (float)np;   // Sine amplitude
        if (mag > best) { best = mag; bestHz = fv; }
      }
      // This component's share of the residual variance; real vibrato is very concentrated
      float share = (tot > 1e-9f) ? (0.5f * best * best) / tot : 0.0f;
      if (share > 0.25f && best > 2.0f) {
        // The 0.5 s moving average itself also shaves off part of the vibrato
        // (attenuation = |sinc(f * 0.5s)|), so divide it back out. Positive control:
        // injecting 12/30 cents measured 11.2/26.6 without the compensation.
        float xw = (float)M_PI * bestHz * (2.0f * (float)HW + 1.0f) / fsD;
        float sc = (fabsf(xw) > 1e-4f) ? sinf(xw) / xw : 1.0f;
        float g  = 1.0f - fabsf(sc);
        if (g > 0.3f) best /= g;
        out.vibratoCents = tc_clampf(best, 0.0f, 60.0f);
        out.vibratoHz    = bestHz;
        Serial.printf("[ANA] 顫音 %.1f cents @ %.1f Hz（佔音高擺動的 %.0f%%）\n",
                      out.vibratoCents, bestHz, share * 100.0f);
      } else {
        Serial.printf("[ANA] 無顫音（音高擺動 %.1f cents，但沒有 3~9 Hz 的週期性）\n",
                      sqrtf(tot));
      }
    }
  }

  // ----------------------- 3b) Fine attack analysis (heterodyne) ------------
  // The FFT cannot see "the high harmonics only arrive 20 ms later", yet that is the
  // main cue the ear uses to identify an instrument.
  heterodyneAttack(wav, (uint32_t)onset * TC_HOP, out.f0, sr, TC_N_HARM);

  {
    float t1 = onsetTimeOf(0);
    if (t1 < 0.0f) t1 = 0.0f;
    for (int h = 0; h < TC_N_HARM; h++) {
      float th = onsetTimeOf(h);
      out.harmOnset[h] = (th < 0.0f) ? 0.0f : tc_clampf(th - t1, 0.0f, 0.15f);
    }
    // 3-point median filter: an individual harmonic occasionally gets misjudged by
    // noise, and only once the outliers are filtered out does the overall shape of the
    // attack become audible (instead of turning into random jitter)
    {
      float tmp[TC_N_HARM];
      for (int h = 0; h < TC_N_HARM; h++) {
        float a = out.harmOnset[h > 0 ? h - 1 : 0];
        float b = out.harmOnset[h];
        float c = out.harmOnset[h < TC_N_HARM - 1 ? h + 1 : TC_N_HARM - 1];
        float mx = fmaxf(a, fmaxf(b, c)), mn = fminf(a, fminf(b, c));
        tmp[h] = a + b + c - mx - mn;                 // Median
      }
      for (int h = 0; h < TC_N_HARM; h++) out.harmOnset[h] = tmp[h];
    }
    // Then average that half-and-half with a fit of "delay grows linearly with harmonic
    // number". The 50% detection on weak harmonics is unstable to begin with, and the
    // measured spread is far larger than the truth; blending in the trend line keeps the
    // perceptual cue of "high harmonics come in late" without letting it jump around.
    {
      float sx = 0, sy = 0, sxx = 0, sxy = 0;
      int   n = 0;
      for (int h = 0; h < TC_N_HARM; h++) {
        if (out.harmOnset[h] <= 0.0f && h > 0) continue;   // Skip the ones that were not measured
        sx += h; sy += out.harmOnset[h];
        sxx += (float)h * h; sxy += h * out.harmOnset[h];
        n++;
      }
      float slope = 0.0f;
      if (n >= 6) {
        float den = n * sxx - sx * sx;
        if (fabsf(den) > 1e-6f) slope = (n * sxy - sx * sy) / den;
      }
      if (slope < 0.0f) slope = 0.0f;                 // Only allow "the higher the harmonic, the later"
      for (int h = 0; h < TC_N_HARM; h++) {
        float fit = slope * h;
        out.harmOnset[h] = tc_clampf(0.5f * out.harmOnset[h] + 0.5f * fit, 0.0f, 0.06f);
      }
    }
    // Re-estimate the attack time from the heterodyne envelope: far more accurate than RMS at 11.6 ms resolution
    float tot[ATK_FRAMES];
    float mx = 0.0f;
    int   mxIdx = 0;
    for (int i = 0; i < ATK_FRAMES; i++) {
      tot[i] = 0.0f;
      for (int h = 0; h < TC_N_HARM; h++) tot[i] += gAtkEnv[h][i];
      if (tot[i] > mx) { mx = tot[i]; mxIdx = i; }
    }
    // A peak at the end of the window means the attack is longer than the window; keep the original RMS estimate in that case
    if (mx > 1e-7f && mxIdx < ATK_FRAMES - 3) {
      int i = 0;
      while (i < ATK_FRAMES && tot[i] < 0.9f * mx) i++;
      float t = (float)i * TC_ATK_HOP / sr;
      out.attack = tc_clampf(t, 0.002f, 0.5f);
    }
    Serial.printf("[ANA] 起音 %.1f ms，高次諧波最大延遲 %.1f ms\n",
                  out.attack * 1000.0f,
                  out.harmOnset[TC_N_HARM - 1] * 1000.0f);
  }

  // ------------------ 3c) Attack transient (strike / bow / breath noise) ----
  // This is the ear's strongest cue for identifying an instrument, and it was the very
  // first thing this program missed.
  //
  // The old version estimated it per bin as "total energy − harmonic energy" out of the
  // FFT, but the FFT window is 46 ms long, so a 6 ms string strike is diluted to almost
  // nothing (material with a true value of 35% read as 0.000).
  // Now it uses the heterodyne residual directly: total energy minus the energy of all
  // the harmonics is the inharmonic part, at 2.9 ms time resolution, and short
  // transients can no longer hide.
  {
    out.attackNoise     = periodicNoiseRatio(wav, (uint32_t)onset * TC_HOP, out.f0, sr, 0.030f);
    // Where the attack noise lands has to be measured too. Attack residual measured on
    // real material (C4):
    //   piano 93% below 1.3 kHz, nothing above 3 kHz
    //   flute 84% in 1.3~3 kHz
    // The synth used to use a hard-coded 9 kHz broadband for everything, i.e. spraying a
    // blob of high-frequency hiss onto every piano onset that was never there --
    // which dropped the piano's centroid correlation from 0.968 to 0.848.
    out.attackHighFrac  = noiseHighFraction(wav, (uint32_t)onset * TC_HOP, out.f0, sr);
    // The sustain noise is now measured the same way (the old FFT approach got eaten by
    // the harmonic bandwidth).
    //
    // But a single 30 ms window will not do: 30 ms is only 8 periods of piano C4, and the
    // estimate is very jittery. Measured per-note noise level errors bounced around
    // between -5.0 and +7.1 dB, and the centroid correlation fell from 0.99 to 0.61 with
    // them. Now it takes 5 points along the sustain and uses the median, so outliers
    // cannot dominate.
    //
    // ★ The sampling window has to land where the note really is sustaining (TC-GUITAR)
    //
    // susA..susB is the "body", and on a decaying instrument that is a very short
    // stretch: on this batch of guitar material D3's body is 0.00~0.46 s, so all 5
    // sample points land in the ringing right after the pluck. The residual ratio is
    // naturally high there (the string has not settled yet) and measured 0.8%; the same
    // note between 0.4 and 1.6 s is only 0.06% -- a factor of 13.
    //
    // The synth takes that number as "the noise level of the whole sustain", so every
    // guitar note carries a layer of hiss no real guitar has, from beginning to end.
    // The noise level is off by +12 dB, and all three of LSD, the spectrogram and the
    // centroid are completely blind to it (they were never sensitive to anything at
    // -70 dB); only evaluate.py's noise-level column catches it.
    //
    // The attack stretch is already attackNoise's job (the first 150 ms, 30 ms time
    // constant), so using it to represent the sustain is double counting. So the window
    // is pushed later:
    //   start: 0.35 s after the attack (skips the ringing from the pluck/strike)
    //   end:   where the level falls to -25 dB below peak (any lower and we are measuring
    //          the recording's own noise floor rather than the instrument; measured,
    //          guitar D3's residual ratio starts climbing again past -20 dB)
    // If 0.2 s cannot be found, fall back to the original body window (short notes, or a
    // take that is quiet throughout).
    //
    // Only applied to decaying instruments. For sustaining ones (flute/trumpet/violin)
    // the body IS the sustain, so the window is already right -- leave it alone;
    // measured, pushing their windows later made every noise-placement error 0.3~1.0
    // percentage points worse (the end of the window swallowed the bow/breath release).
    // This is not "branching on instrument type": decaying is measured from the envelope
    // (body under 25% of the note length and peak within the first 25%), and other parts
    // of analyzer (release, decay rate) have been branching on that same quantity all
    // along.
    {
      int nsA = susA, nsB = susB;
      if (decaying) {
        const int a = onset + (int)(0.35f / frameSec);
        int b = (a > susB) ? a : susB;
        while (b + 1 <= offset && gRms[b + 1] > 0.056f) b++;    // -25 dB (gRms is already normalised)
        if (a < b && (b - a) >= (int)(0.20f / frameSec)) { nsA = a; nsB = b; }
      }
      const int NP = 5;
      float ng[NP], nh[NP];
      int   cnt = 0;
      for (int i = 0; i < NP; i++) {
        int fr = nsA + (nsB - nsA) * i / (NP - 1 > 0 ? NP - 1 : 1);
        uint32_t pos = (uint32_t)fr * TC_HOP;
        if (pos + NFFT > N) break;
        ng[cnt] = periodicNoiseRatio(wav, pos, out.f0, sr, 0.030f);
        nh[cnt] = noiseHighFraction(wav, pos, out.f0, sr);
        cnt++;
      }
      if (cnt == 0) {
        uint32_t susPos = (uint32_t)((susA + susB) / 2) * TC_HOP;
        out.noiseGain     = periodicNoiseRatio(wav, susPos, out.f0, sr, 0.030f);
        out.noiseHighFrac = noiseHighFraction(wav, susPos, out.f0, sr);
      } else {
        qsort(ng, cnt, sizeof(float), cmpf);
        qsort(nh, cnt, sizeof(float), cmpf);
        out.noiseGain     = ng[cnt / 2];
        out.noiseHighFrac = nh[cnt / 2];
      }
      Serial.printf("[ANA] 噪聲落點：%.0f%% 在 5*f0 以上（長笛約 20%%、提琴約 90%%、鋼琴約 2%%）\n",
                    out.noiseHighFrac * 100.0f);
    }
    Serial.printf("[ANA] 非諧波比例：起音 %.1f %%  持續 %.1f %%\n",
                  out.attackNoise * 100.0f, out.noiseGain * 100.0f);
  }

  // ------------------------- 4) Per-bin FFT: harmonics + spectral envelope --
  memset(gHarmAcc, 0, sizeof(gHarmAcc));
  memset(gLoudAcc, 0, sizeof(gLoudAcc));
  memset(gCnt,     0, sizeof(gCnt));
  memset(gAvgMag,  0, sizeof(gAvgMag));

  File csv;
  if (csvDumpPath) {
    if (SD.exists(csvDumpPath)) SD.remove(csvDumpPath);
    csv = SD.open(csvDumpPath, FILE_WRITE);
    if (csv) {
      csv.print(F("t,f0,loud"));
      for (int h = 1; h <= TC_N_HARM; h++) { csv.print(F(",h")); csv.print(h); }
      csv.println(F(",noise"));
    }
  }

  // The last 20% counts as "released", matching the MLP's 4th input feature
  const int   relStart  = onset + (int)((offset - onset) * 0.80f);
  const float binHz     = sr / NFFT;
  const int   nBins     = NFFT / 2;
  const float winGain   = 2.0f / (NFFT * 0.5f);        // Hann coherent gain compensation
  float noiseAcc = 0.0f;
  // Inharmonicity is fitted with two-parameter least squares (see the notes in 4b
  // below); this accumulates the sums it needs.
  // Using double: Sxx builds up to the 10^8 range, where float's 24-bit mantissa is no
  // longer enough. This is offline analysis, run once per note, so the cost of software
  // double on the M7 is negligible.
  double inhN = 0.0, inhSx = 0.0, inhSy = 0.0, inhSxx = 0.0, inhSxy = 0.0, inhSyy = 0.0;
  int   avgN     = 0;
  float atkNoiseAcc = 0.0f;                 // Noise ratio in the 30 ms before the attack
  int   atkNoiseN   = 0;
  const int atkNoiseEnd = onset + (int)(0.030f * sr / TC_HOP) + 1;
  int   shimN = 0;                          // How far shimmer tracking has got

  for (int f = onset; f <= offset; f++) {
    uint32_t pos = (uint32_t)f * TC_HOP;
    if (pos + NFFT > N) break;
    wav.readMono(pos, gBuf, NFFT);

    // Report progress every 16 bins (the callback may have to redraw the OLED, so do not call it too often)
    if (gProgressCb && ((f - onset) & 15) == 0)
      gProgressCb((float)(f - onset) / (float)(offset - onset + 1));

    for (int i = 0; i < NFFT; i++) { gRe[i] = gBuf[i] * gWin[i]; gIm[i] = 0.0f; }
    fft();
    float total = 0.0f;
    for (int i = 0; i < nBins; i++) {
      gMag[i] = sqrtf(gRe[i] * gRe[i] + gIm[i] * gIm[i]);
      total  += gMag[i] * gMag[i];
    }

    // ---- Noise floor: median of bins midway between adjacent harmonics -----
    // Those positions are definitely not harmonics, so they reflect the floor directly.
    // Without this gate, high harmonics buried in the floor get taken for real signal and
    // pick up 1~3% of the normalised weight, which adds a layer of high-frequency "fuzz"
    // no real instrument has.
    float noiseFloor;
    {
      float mid[TC_N_HARM];
      int   nm = 0;
      for (int h = 0; h < TC_N_HARM; h++) {
        float fm = (h + 1.5f) * out.f0;
        if (fm > sr * 0.48f) break;
        int b = (int)(fm / binHz + 0.5f);
        if (b > 0 && b < nBins) mid[nm++] = gMag[b];
      }
      if (nm >= 4) {
        qsort(mid, nm, sizeof(float), cmpf);
        noiseFloor = mid[nm / 2];
      } else {
        noiseFloor = 0.0f;
      }
    }

    // Harmonic extraction
    float amp[TC_N_HARM];
    float harmEnergy = 0.0f;
    // Bin half-width per harmonic: must not exceed half the spacing between adjacent harmonics, or the low notes get counted twice
    int   halfW = (int)(out.f0 / binHz * 0.5f);
    if (halfW > 2) halfW = 2;
    if (halfW < 1) halfW = 1;

    for (int h = 0; h < TC_N_HARM; h++) {
      float fh = (h + 1) * out.f0;
      if (fh > sr * 0.48f) { amp[h] = 0.0f; continue; }
      float exactBin;
      // The search radius follows "how far this harmonic could have wandered":
      //   3.5% of fh  -> covers the shift for B up to 0.0005 (piano is about 0.0003)
      //   0.45 * f0   -> hard limit, must not touch the neighbouring harmonic
#if TC_PEAK_WIDE
      int rad = (int)fminf(0.035f * fh / binHz, 0.45f * out.f0 / binHz);
      if (rad < 2)  rad = 2;
      if (rad > 12) rad = 12;
#else
      int rad = 2;
#endif
      float m = peakAt(fh / binHz, nBins, &exactBin, rad);
      // Not clearly above the floor means the harmonic is simply not there
      amp[h]  = (m > 2.5f * noiseFloor) ? m * winGain : 0.0f;
      // Actually sum the energy around the peak instead of approximating with a fixed
      // factor. The old m*m*3 often overestimated, driving noiseFrac to 0 and leaving
      // the noise layer effectively disabled.
      {
        int c = (int)(exactBin + 0.5f);
        int b0 = c - halfW, b1 = c + halfW;
        if (b0 < 0) b0 = 0;
        if (b1 > nBins - 1) b1 = nBins - 1;
        for (int b = b0; b <= b1; b++) harmEnergy += gMag[b] * gMag[b];
      }

      // Inharmonicity: accumulate the pair (n^2, ratio^2 - 1) and fit them all together
      // at the end. Why not "one B per harmonic, then average" like the old version --
      // see 4b below.
      if (h >= 3 && h < 12 && m > 1e-5f) {
        const double n2 = (double)(h + 1) * (h + 1);
        const double ratio = (double)(exactBin * binHz) / fh;
        const double y = ratio * ratio - 1.0;
        if (y > -0.30 && y < 0.60) {          // Obviously mis-picked peaks stay out of the regression
          inhN   += 1.0;
          inhSx  += n2;      inhSy  += y;
          inhSxx += n2 * n2; inhSxy += n2 * y;
          inhSyy += y * y;
        }
      }
    }
    float noiseFrac = (total > 1e-12f) ? tc_clampf(1.0f - harmEnergy / total, 0.0f, 1.0f) : 0.0f;
    noiseAcc += noiseFrac;
    if (f < atkNoiseEnd) { atkNoiseAcc += noiseFrac; atkNoiseN++; }

    // Per-harmonic tracks over the sustain (for measuring shimmer)
    if (f >= shimA && f <= shimB && shimN < SHIM_MAX) {
      for (int hh = 0; hh < SHIM_HARM; hh++) gShimTrack[hh][shimN] = amp[hh];
      shimN++;
    }

    // Accumulate into keyframes -- on a log time axis, so the attack gets enough bins
    float tSec = (f - onset) * frameSec;
    int k = (int)(tc_timeWarp(tSec, out.noteDur) * TC_N_KEYFRAME);
    if (k < 0) k = 0;
    if (k > TC_N_KEYFRAME - 1) k = TC_N_KEYFRAME - 1;
    for (int h = 0; h < TC_N_HARM; h++) gHarmAcc[k][h] += amp[h];
    gLoudAcc[k] += gRms[f];
    gCnt[k]     += 1.0f;

    // Mean spectrum (sustain), used to build the spectral envelope
    if (f >= susA && f <= susB) {
      for (int i = 0; i < nBins; i++) gAvgMag[i] += gMag[i];
      avgN++;
    }

    // ---- Training samples (shared by CSV export and on-device training) ----
    if (csv || trainSet) {
      float sum = 0.0f;
      for (int h = 0; h < TC_N_HARM; h++) sum += amp[h];
      if (sum > 1e-9f) {
        float normAmp[TC_N_HARM];
        for (int h = 0; h < TC_N_HARM; h++) normAmp[h] = amp[h] / sum;
        float noiseFrac2 = noiseFrac;
        // The MLP's time feature has to use the same log time axis, or training and inference will not line up
        float tNorm = tc_timeWarp(tSec, out.noteDur);

        if (csv) {
          csv.printf("%.4f,%.2f,%.4f", tSec, out.f0, gRms[f]);
          for (int h = 0; h < TC_N_HARM; h++) csv.printf(",%.6f", normAmp[h]);
          csv.printf(",%.4f\n", noiseFrac2);
        }
        if (trainSet) {
          float in[TC_MLP_IN];
          in[0] = tc_clampf(log2f(out.f0 / 261.63f) / TC_MLP_PITCH_SCALE, -3.0f, 3.0f);
          in[1] = tc_clampf(gRms[f], 0.0f, 1.0f);
          in[2] = tc_clampf(tNorm, 0.0f, 1.5f);   // Already on the log time axis
          in[3] = (f >= relStart) ? 1.0f : 0.0f;
          trainSet->add(in, normAmp, noiseFrac2);
        }
      }
    }
  }
  if (csv) csv.close();

  // noiseGain / attackNoise were already measured with the periodic residual in 3c); do not overwrite them here.

  // ----------------- 4b) Inharmonicity: pitch error vs. real inharmonicity --
  //
  // The old version computed B = (ratio^2 - 1) / n^2 per harmonic and averaged. That
  // formula translates "f0 estimated wrong" straight into inharmonicity: if f0 is low by
  // δ, every harmonic looks high by the same fraction, so ratio^2 - 1 ≈ 2δ for all n,
  // and dividing by n^2 still leaves it positive, so the average becomes a fake positive
  // B. And the final clamp has a lower bound of 0, so negative values get clipped and
  // positive ones survive -- the bias is one-directional: it can only overestimate,
  // never underestimate.
  //
  // Measured consequence: the Iowa violin material (where B should physically be ~0)
  // measured C#4 = 0.00027 and G4 = 0.00035, which is piano territory. At B = 0.00035
  // the 20th harmonic is 113 cents sharp and the 32nd is 265 cents sharp; sound three
  // voices together and you get one sheet of hiss.
  //
  // The right way is to fit two things at once:
  //
  //     ratio^2 - 1  =  c  +  B * n^2
  //                     ^     ^
  //                     |     +-- real inharmonicity: offset grows as n^2
  //                     +-------- f0 estimate error: constant across all n
  //
  // This is a standard simple linear regression (independent variable x = n^2). c
  // absorbs the pitch error entirely, which makes B immune to the f0 estimation error --
  // exactly the term the old version was missing.
  //
  // Plus a significance test: B is only believed when it exceeds twice its own standard
  // error, otherwise it is reported as 0. For strings and winds B sits down in the noise
  // anyway, and it is better to say honestly that there is none than to force out a number.
  {
    out.inharmonicity = 0.0f;
    const double N = inhN;
    if (N >= 8.0) {
      const double Sxx = inhSxx - inhSx * inhSx / N;      // Centred sum of squares
      const double Sxy = inhSxy - inhSx * inhSy / N;
      const double Syy = inhSyy - inhSy * inhSy / N;
      if (Sxx > 1e-9) {
        const double B   = Sxy / Sxx;                      // Slope = inharmonicity coefficient
        const double sse = Syy - B * Sxy;                  // Residual sum of squares
        const double se  = sqrt(fmax(sse, 0.0) / ((N - 2.0) * Sxx));   // Standard error of B
        // Only believed if significant and positive. If se is 0 (a perfect fit), accept it outright.
        const bool sig = (se <= 0.0) || (B > 2.0 * se);
        if (sig && B > 0.0) out.inharmonicity = tc_clampf((float)B, 0.0f, 0.002f);
        Serial.printf("[ANA] 非諧性擬合：B = %.6f ± %.6f（n=%d）%s\n",
                      B, se, (int)N, out.inharmonicity > 0.0f ? "" : " -> 不顯著，判 0");
      }
    }
  }
  (void)noiseAcc; (void)atkNoiseAcc; (void)atkNoiseN;

  // ---------------------------- shimmer (micro-fluctuation in the sustain) --
  // What we want is "fast jitter around the trend", not "the note is decaying".
  //
  // The old version detrended with a single exponential fit, but on a fast-decaying
  // instrument like the piano the decay is nothing like a single exponential (the high
  // harmonics fall several times faster than the fundamental), so the residual gets
  // mistaken for fluctuation -- the piano measured 22.6% shimmer, and synthesis added a
  // huge fake tremble that sounded like a synth pad and homogenised piano and violin
  // into the same sound.
  //
  // Now it divides by a local moving average: whatever shape the decay has, it is
  // removed completely, leaving purely the jitter that is faster than the window.
  if (shimN >= 24) {
    const int W = 9;                            // Moving average of about 104 ms
    float acc = 0.0f;
    int   accN = 0;
    for (int h = 0; h < SHIM_HARM; h++) {
      // Harmonics that decay too fast are excluded: measuring a 27/s decay with a 46 ms
      // FFT window, the amplitude already falls 70% inside the window, so the
      // "fluctuation" measured is really measurement error.
      // That is exactly how the piano's high harmonics got mistaken for shimmer.
      {
        float first = gShimTrack[h][W / 2];
        float last  = gShimTrack[h][shimN - W / 2 - 1];
#ifdef TC_SHIM_DEBUG
        Serial.printf("      [shim] h%-2d first=%.3e last=%.3e ratio=%.3f %s\n",
                      h, first, last, (first > 1e-12f ? last / first : 0.0f),
                      (first < 1e-8f) ? "REJ:first~0"
                    : (last < 0.25f * first) ? "REJ:decayed" : "ok");
#endif
        if (first < 1e-8f || last < 0.25f * first) continue;
      }
      float m = 0.0f, m2 = 0.0f;
      int   k = 0;
      for (int i = W / 2; i < shimN - W / 2; i++) {
        float v = gShimTrack[h][i];
        if (v < 1e-8f) continue;
        // Log-domain averaging: unbiased for exponential decay.
        // With an ordinary arithmetic mean, the high harmonics decay fast (18/s
        // measured) and the curvature inside the window reads as fluctuation, which gave
        // the piano a fake 10.7% shimmer.
        float ma = 0.0f;
        int   mn = 0;
        for (int j = i - W / 2; j <= i + W / 2; j++) {
          if (gShimTrack[h][j] < 1e-8f) continue;
          ma += logf(gShimTrack[h][j]); mn++;
        }
        if (mn < W - 2) continue;
        ma = expf(ma / mn);
        if (ma < 1e-8f) continue;
        float r = v / ma;                       // Centred on 1 after detrending
        m += r; m2 += r * r; k++;
      }
#ifdef TC_SHIM_DEBUG
      Serial.printf("      [shim] h%-2d usable_frames=%d %s\n", h, k,
                    (k < 16) ? "REJ:k<16" : "counted");
#endif
      if (k < 16) continue;
      m /= k;
      float var = m2 / k - m * m;
      if (var > 0.0f) { acc += sqrtf(var); accN++; }
    }
    // Too few harmonics qualifying means "it cannot be measured" -- then be honest and
    // report 0, rather than letting one or two very noisy surviving harmonics stand for
    // the whole, which would conjure up a fake tremble.
#ifdef TC_SHIM_DEBUG
    Serial.printf("      [shim] shimN=%d accN=%d raw=%.4f\n",
                  shimN, accN, (accN > 0) ? acc / accN : -1.0f);
#endif
    out.shimmerDepth = (accN >= 4) ? tc_clampf(acc / accN, 0.0f, TC_SHIM_MAX) : 0.0f;
    if (accN < 4) Serial.println(F("[ANA] 持續段太短或衰減太快，shimmer 判定為 0"));
  } else {
#ifdef TC_SHIM_DEBUG
    Serial.printf("      [shim] shimN=%d < 24 -> 0\n", shimN);
#endif
    out.shimmerDepth = 0.0f;
  }

  // ----------------------------------------- 5) Keyframe normalisation ------
  float lastGood[TC_N_HARM];
  for (int h = 0; h < TC_N_HARM; h++) lastGood[h] = (h == 0) ? 1.0f : 0.0f;

  for (int k = 0; k < TC_N_KEYFRAME; k++) {
    if (gCnt[k] > 0.5f) {
      float sum = 0.0f;
      for (int h = 0; h < TC_N_HARM; h++) sum += gHarmAcc[k][h];
      if (sum > 1e-9f) {
        for (int h = 0; h < TC_N_HARM; h++) {
          out.keyframe[k][h] = gHarmAcc[k][h] / sum;
          lastGood[h]        = out.keyframe[k][h];
        }
      } else {
        for (int h = 0; h < TC_N_HARM; h++) out.keyframe[k][h] = lastGood[h];
      }
      out.loud[k] = tc_clampf(gLoudAcc[k] / gCnt[k], 0.0f, 1.0f);
    } else {
      for (int h = 0; h < TC_N_HARM; h++) out.keyframe[k][h] = lastGood[h];
      out.loud[k] = (k > 0) ? out.loud[k - 1] : 0.0f;
    }
  }

  // ------------------ 5b) Sustaining: remove the player's crescendo ---------
  //
  // For bowed strings and winds, the level changes in the recording are **performance
  // expression**, not an intrinsic property of the instrument. All 12 measured violin
  // files are crescendos (0.3 -> 1.0); play that back literally and every synthesised
  // note swells on its own, which is useless for playing a melody.
  //
  // So for sustaining instruments only the attack is kept (that is the instrument's
  // signature: how long the bow takes to speak, the bow noise), and everything after it
  // is flattened to the body's median. Decaying instruments are left alone -- their
  // decay is the sound itself.
  if (!decaying) {
    // Body median
    float tmp[TC_N_KEYFRAME];
    int   n = 0;
    for (int k = 0; k < TC_N_KEYFRAME; k++)
      if (out.loud[k] > 0.01f) tmp[n++] = out.loud[k];
    float med = 0.7f;
    if (n >= 4) { qsort(tmp, n, sizeof(float), cmpf); med = tmp[n / 2]; }

    // End of attack = the first point that reaches 0.9x the median
    int kAtk = 0;
    while (kAtk < TC_N_KEYFRAME - 1 && out.loud[kAtk] < 0.9f * med) kAtk++;
    for (int k = kAtk; k < TC_N_KEYFRAME; k++) out.loud[k] = med;

    Serial.printf("[ANA] 持續型：起音佔 %d/%d 格，之後壓平到 %.2f（移除演奏者的漸強）\n",
                  kAtk, TC_N_KEYFRAME, med);
  }

  // ------------------------------------------ 6) log spectral envelope ------
  if (avgN > 0) {
    for (int i = 0; i < nBins; i++) gAvgMag[i] /= avgN;

    const float lo = logf(TC_SPECENV_FMIN), hi = logf(TC_SPECENV_FMAX);
    float envMax = -300.0f;
    for (int p = 0; p < TC_SPECENV_PTS; p++) {
      float fc  = expf(lo + (hi - lo) * p / (TC_SPECENV_PTS - 1));
      // The window width needs a floor of 0.6*f0 (total width 1.2*f0), otherwise the
      // "envelope" in the low bands still carries the comb structure of the harmonics --
      // ±12% is far narrower than the harmonic spacing when fc is small, and the window
      // drops right into the valley between two harmonics.
      //
      // Measured on trumpet B4 (f0=494.6): looking up 698 Hz (exactly between h1 and h2)
      // gives -55.8 dB, so on transposing to F5 the fundamental's correction gain becomes
      // 0.004, squashing it by 48 dB -- the synthesised F5 fundamental is left at 1.0%
      // where the true value is 30%.
      // That also explains why the error is non-monotonic in transposition distance: an
      // octave lands on a harmonic and is fine, a tritone lands in a valley and collapses.
      //
      // The floor is 0.6*f0 rather than anything larger because wider smears out the
      // high-frequency detail (measured, the 2.5k~5k rolloff keeps 18.0 dB at 0.6, and
      // starts deforming above 0.8).
      float w   = fmaxf(fmaxf(fc * 0.12f, binHz * 1.5f), out.f0 * 0.6f);
      int   b0  = (int)((fc - w) / binHz), b1 = (int)((fc + w) / binHz);
      if (b0 < 1) b0 = 1;
      if (b1 > nBins - 1) b1 = nBins - 1;
      float mx = 0.0f;
      for (int b = b0; b <= b1; b++) if (gAvgMag[b] > mx) mx = gAvgMag[b];
      float db = 20.0f * log10f(mx + 1e-9f);
      out.specEnv[p] = db;
      if (db > envMax) envMax = db;
    }
    for (int p = 0; p < TC_SPECENV_PTS; p++)
      out.specEnv[p] = tc_clampf(out.specEnv[p] - envMax, -72.0f, 0.0f);   // Peak taken as 0 dB
  }

  // Brightness (spectral centroid / f0), purely for human consumption
  {
    float num = 0.0f, den = 0.0f;
    const int mid = TC_N_KEYFRAME / 2;
    for (int h = 0; h < TC_N_HARM; h++) { num += (h + 1) * out.keyframe[mid][h]; den += out.keyframe[mid][h]; }
    out.brightness = den > 1e-9f ? num / den : 1.0f;
  }

  out.magic = TC_PROFILE_MAGIC;
  out.valid = true;
  wav.close();

  Serial.printf("[ANA] 完成，耗時 %lu ms\n", (unsigned long)(millis() - t0));
  profilePrint(out);
  return true;
}

#include "analyzer.h"
#include "wav_io.h"
#include "trainer.h"
#include <SD.h>

#define NFFT   TC_FFT_SIZE
#define NBITS  11
static_assert(NFFT == 2048, "NBITS 要跟著 TC_FFT_SIZE 改");

DMAMEM static float  gRe[NFFT];
DMAMEM static float  gIm[NFFT];
DMAMEM static float  gTwRe[NFFT / 2];
DMAMEM static float  gTwIm[NFFT / 2];
DMAMEM static float  gWin[NFFT];
DMAMEM static float  gMag[NFFT / 2];
DMAMEM static float  gAvgMag[NFFT / 2];
DMAMEM static float  gRms[TC_MAX_FRAMES];

static int gOnsetCount = 1;
int analyzerLastOnsetCount() { return gOnsetCount; }

static float gPeakAbs    = 0.0f;
static float gClipRatio  = 0.0f;
static float gNoiseFloor = 0.0f;
float analyzerLastPeak()       { return gPeakAbs;    }
float analyzerLastClipRatio()  { return gClipRatio;  }
float analyzerLastNoiseFloor() { return gNoiseFloor; }
DMAMEM static float  gYin[NFFT / 2];
DMAMEM static float  gHarmAcc[TC_N_KEYFRAME][TC_N_HARM];
DMAMEM static float  gLoudAcc[TC_N_KEYFRAME];
DMAMEM static float  gCnt[TC_N_KEYFRAME];
DMAMEM static float  gBuf[NFFT];

#define ATK_FRAMES  ((int)(TC_ATK_WINDOW_SEC * TC_SAMPLE_RATE / TC_ATK_HOP))
DMAMEM static float  gAtkEnv[TC_N_HARM][ATK_FRAMES];
DMAMEM static float  gAtkTot[ATK_FRAMES];

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
  for (int i = 0; i < NFFT; i++)
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

static float yinPitch(const float *x, float sr) {
  const int W      = NFFT / 2;
  const int tauMin = (int)(sr / TC_F0_MAX);
  const int tauMax = (int)(sr / TC_F0_MIN);
  if (tauMax >= W) return 0.0f;

  gYin[0] = 1.0f;
  for (int tau = 1; tau <= tauMax; tau++) {
    float s = 0.0f;
    for (int j = 0; j < W; j++) { float d = x[j] - x[j + tau]; s += d * d; }
    gYin[tau] = s;
  }

  float run = 0.0f;
  for (int tau = 1; tau <= tauMax; tau++) {
    run += gYin[tau];
    gYin[tau] = (run > 1e-12f) ? gYin[tau] * tau / run : 1.0f;
  }

  int best = -1;
  for (int tau = tauMin; tau < tauMax; tau++) {
    if (gYin[tau] < TC_YIN_THRESH) {
      while (tau + 1 < tauMax && gYin[tau + 1] < gYin[tau]) tau++;
      best = tau;
      break;
    }
  }
  if (best < 0) {
    float m = 1e30f;
    for (int tau = tauMin; tau < tauMax; tau++)
      if (gYin[tau] < m) { m = gYin[tau]; best = tau; }
    if (best < 0 || m > 0.6f) return 0.0f;
  }

  float betterTau = (float)best;
  if (best > 0 && best < tauMax - 1) {
    float a = gYin[best - 1], b = gYin[best], c = gYin[best + 1];
    float den = 2.0f * (2.0f * b - a - c);
    if (fabsf(den) > 1e-9f) betterTau = best + (c - a) / den;
  }
  return sr / betterTau;
}

static void heterodyneAttack(WavReader &wav, uint32_t startSample,
                             float f0, float sr, int nHarm) {
  const float lpA = 1.0f - expf(-2.0f * (float)M_PI * TC_ATK_LP_HZ / sr);
  const int   nSamp = (int)(TC_ATK_WINDOW_SEC * sr);

  static float pr[TC_N_HARM], pi[TC_N_HARM];
  static float lr[TC_N_HARM], li[TC_N_HARM];
  static float cw[TC_N_HARM], sw[TC_N_HARM];
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

  float totLp = 0.0f;
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
      if ((n & 255) == 255) {
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

static float periodicNoiseRatio(WavReader &wav, uint32_t startSample,
                                float f0, float sr, float windowSec) {
  const float Tf0 = sr / f0;
  const int   nWin = (int)(windowSec * sr);
  const int   need = nWin + (int)(Tf0 * 1.03f) + 2;
  if (need > NFFT) return 0.0f;

  if (wav.readMono(startSample, gBuf, need) < (uint32_t)need) return 0.0f;

  float best = 1.0f;

  for (int k = -3; k <= 3; k++) {
    const float Tf   = Tf0 * (1.0f + 0.01f * k);
    const int   Ti   = (int)Tf;
    const float frac = Tf - Ti;
    if (Ti < 2 || Ti + 1 + nWin > need) continue;

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
      const float g = (float)sqrt(ea / eb);
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

static float noiseHighFraction(WavReader &wav, uint32_t startSample,
                               float f0, float sr) {
  const int Ti = (int)(sr / f0 + 0.5f);
  const int nWin = NFFT - Ti - 2;
  if (nWin < 1024) return 0.5f;
  if (wav.readMono(startSample, gBuf, NFFT) < (uint32_t)NFFT) return 0.5f;

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

static int cmpf(const void *a, const void *b) {
  float d = *(const float *)a - *(const float *)b;
  return (d > 0) - (d < 0);
}

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
  return b - 0.25f * (a - cc) * d;
}

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
  for (int f = 0; f < nFrames; f++) gRms[f] /= rmsMax;

  int onset = 0;
  while (onset < nFrames && gRms[onset] < 0.08f) onset++;
  int offset = nFrames - 1;
  while (offset > onset && gRms[offset] < 0.04f) offset--;
  if (offset - onset < 4) { Serial.println(F("[ANA] 有效音長太短")); wav.close(); return false; }

  int peakIdx = onset;
  for (int f = onset; f <= offset; f++) if (gRms[f] > gRms[peakIdx]) peakIdx = f;

  if (onset > 0) {
    float s = 0.0f;
    for (int f = 0; f < onset; f++) s += gRms[f];
    gNoiseFloor = s / onset;
  }

  const float frameSec = TC_HOP / sr;
  out.noteDur = (offset - onset) * frameSec;

  {
    const int minGap = (int)(0.15f / frameSec + 0.5f);
    int   count = 0, last = -9999;
    float runMin = 1e9f;
    for (int f = peakIdx + 1; f <= offset; f++) {
      if (gRms[f] < runMin) runMin = gRms[f];
      if (f - last < minGap) continue;
      if (gRms[f] < 0.25f)  continue;
      if (runMin > 1e-4f && gRms[f] / runMin >= 3.0f) {
        count++; last = f; runMin = gRms[f];
      }
    }

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

  out.attack = fmaxf((peakIdx - onset) * frameSec, 0.003f);

  int bodyA = onset, bodyB = offset;
  {
    int f = onset;
    while (f <= offset && gRms[f] < 0.6f) f++;
    if (f <= offset) bodyA = f;
    f = offset;
    while (f > bodyA && gRms[f] < 0.6f) f--;
    bodyB = f;
  }

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

  const float peakPos = (float)(peakIdx - onset) / (float)(offset - onset + 1);
  const bool  decaying = (bodyFrac < 0.25f) && (peakPos < 0.25f);

  int susA = bodyA, susB = bodyB;

  int shimA = susA, shimB = susB;
  if (!decaying) {
    const int a = onset + (int)(TC_SHIM_START_SEC / frameSec);
    if (a < shimA) shimA = a;
  }
  {
    static float tmp[TC_MAX_FRAMES];
    int n = 0;
    for (int f = susA; f <= susB && n < TC_MAX_FRAMES; f++) tmp[n++] = gRms[f];
    qsort(tmp, n, sizeof(float), cmpf);
    out.sustain = tc_clampf(tmp[n / 2], 0.02f, 1.0f);
  }

  {
    int f = bodyA;
    while (f < bodyB && gRms[f] > out.sustain * 1.05f) f++;
    out.decay = fmaxf((f - bodyA) * frameSec, 0.01f);
  }

  out.release = decaying ? 0.20f
                         : fmaxf((offset - bodyB) * frameSec, 0.10f);

  {
    int rA = bodyA;
    int rB = decaying ? offset : bodyB;
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

    if (decaying) perSec = tc_clampf(perSec, 0.05f, 0.97f);
    else          perSec = tc_clampf(perSec, 0.95f, 1.0f);
    out.sustainDecayPerSec = (perSec > 0.97f) ? 1.0f : perSec;
  }

  out.envHoldNorm = 1.0f;

  Serial.printf("[ANA] 本體 %.2f~%.2f s（佔音長 %.0f%%）-> 判定為%s\n",
                (bodyA - onset) * frameSec, (bodyB - onset) * frameSec,
                bodyFrac * 100.0f, decaying ? "衰減型" : "持續型");

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

  {

    int m = (int)(out.f0 / 130.0f + 0.5f);
    if (m < 1) m = 1;
    const int   DECIM = (int)(m * sr / out.f0 + 0.5f);
    const int   MAXP = 256;
    const float fsD   = sr / (float)DECIM;
    static float trk[MAXP];
    int np = 0;

    const float w  = 2.0f * (float)M_PI * out.f0 / sr;
    const float cw = cosf(w), sw = sinf(w);
    float cr = 1.0f, ci = 0.0f;
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

        float nr = cr * cw - ci * sw;
        ci = cr * sw + ci * cw;
        cr = nr;
        if (((i & 1023) == 1023)) {
          float g = 1.5f - 0.5f * (cr * cr + ci * ci);
          cr *= g; ci *= g;
        }
        if (++cnt >= DECIM) {
          if (havePrev) {

            float re = accI * pI + accQ * pQ;
            float im = accQ * pI - accI * pQ;
            float dphi = atan2f(im, re);
            trk[np++] = 1731.2f * (dphi * fsD / 6.2831853f) / out.f0;
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

      float best = 0.0f, bestHz = 0.0f;
      for (float fv = 3.0f; fv <= 9.01f; fv += 0.25f) {
        float re = 0.0f, im = 0.0f;
        float ph = 0.0f, dp = 2.0f * (float)M_PI * fv / fsD;
        for (int i = 0; i < np; i++) { re += det[i] * cosf(ph); im -= det[i] * sinf(ph); ph += dp; }
        float mag = 2.0f * sqrtf(re * re + im * im) / (float)np;
        if (mag > best) { best = mag; bestHz = fv; }
      }

      float share = (tot > 1e-9f) ? (0.5f * best * best) / tot : 0.0f;
      if (share > 0.25f && best > 2.0f) {

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

  heterodyneAttack(wav, (uint32_t)onset * TC_HOP, out.f0, sr, TC_N_HARM);

  {
    float t1 = onsetTimeOf(0);
    if (t1 < 0.0f) t1 = 0.0f;
    for (int h = 0; h < TC_N_HARM; h++) {
      float th = onsetTimeOf(h);
      out.harmOnset[h] = (th < 0.0f) ? 0.0f : tc_clampf(th - t1, 0.0f, 0.15f);
    }

    {
      float tmp[TC_N_HARM];
      for (int h = 0; h < TC_N_HARM; h++) {
        float a = out.harmOnset[h > 0 ? h - 1 : 0];
        float b = out.harmOnset[h];
        float c = out.harmOnset[h < TC_N_HARM - 1 ? h + 1 : TC_N_HARM - 1];
        float mx = fmaxf(a, fmaxf(b, c)), mn = fminf(a, fminf(b, c));
        tmp[h] = a + b + c - mx - mn;
      }
      for (int h = 0; h < TC_N_HARM; h++) out.harmOnset[h] = tmp[h];
    }

    {
      float sx = 0, sy = 0, sxx = 0, sxy = 0;
      int   n = 0;
      for (int h = 0; h < TC_N_HARM; h++) {
        if (out.harmOnset[h] <= 0.0f && h > 0) continue;
        sx += h; sy += out.harmOnset[h];
        sxx += (float)h * h; sxy += h * out.harmOnset[h];
        n++;
      }
      float slope = 0.0f;
      if (n >= 6) {
        float den = n * sxx - sx * sx;
        if (fabsf(den) > 1e-6f) slope = (n * sxy - sx * sy) / den;
      }
      if (slope < 0.0f) slope = 0.0f;
      for (int h = 0; h < TC_N_HARM; h++) {
        float fit = slope * h;
        out.harmOnset[h] = tc_clampf(0.5f * out.harmOnset[h] + 0.5f * fit, 0.0f, 0.06f);
      }
    }

    float tot[ATK_FRAMES];
    float mx = 0.0f;
    int   mxIdx = 0;
    for (int i = 0; i < ATK_FRAMES; i++) {
      tot[i] = 0.0f;
      for (int h = 0; h < TC_N_HARM; h++) tot[i] += gAtkEnv[h][i];
      if (tot[i] > mx) { mx = tot[i]; mxIdx = i; }
    }

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

  {
    out.attackNoise     = periodicNoiseRatio(wav, (uint32_t)onset * TC_HOP, out.f0, sr, 0.030f);

    out.attackHighFrac  = noiseHighFraction(wav, (uint32_t)onset * TC_HOP, out.f0, sr);

    {
      int nsA = susA, nsB = susB;
      if (decaying) {
        const int a = onset + (int)(0.35f / frameSec);
        int b = (a > susB) ? a : susB;
        while (b + 1 <= offset && gRms[b + 1] > 0.056f) b++;
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

  const int   relStart  = onset + (int)((offset - onset) * 0.80f);
  const float binHz     = sr / NFFT;
  const int   nBins     = NFFT / 2;
  const float winGain   = 2.0f / (NFFT * 0.5f);
  float noiseAcc = 0.0f;

  double inhN = 0.0, inhSx = 0.0, inhSy = 0.0, inhSxx = 0.0, inhSxy = 0.0, inhSyy = 0.0;
  int   avgN     = 0;
  float atkNoiseAcc = 0.0f;
  int   atkNoiseN   = 0;
  const int atkNoiseEnd = onset + (int)(0.030f * sr / TC_HOP) + 1;
  int   shimN = 0;

  for (int f = onset; f <= offset; f++) {
    uint32_t pos = (uint32_t)f * TC_HOP;
    if (pos + NFFT > N) break;
    wav.readMono(pos, gBuf, NFFT);

    if (gProgressCb && ((f - onset) & 15) == 0)
      gProgressCb((float)(f - onset) / (float)(offset - onset + 1));

    for (int i = 0; i < NFFT; i++) { gRe[i] = gBuf[i] * gWin[i]; gIm[i] = 0.0f; }
    fft();
    float total = 0.0f;
    for (int i = 0; i < nBins; i++) {
      gMag[i] = sqrtf(gRe[i] * gRe[i] + gIm[i] * gIm[i]);
      total  += gMag[i] * gMag[i];
    }

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

    float amp[TC_N_HARM];
    float harmEnergy = 0.0f;

    int   halfW = (int)(out.f0 / binHz * 0.5f);
    if (halfW > 2) halfW = 2;
    if (halfW < 1) halfW = 1;

    for (int h = 0; h < TC_N_HARM; h++) {
      float fh = (h + 1) * out.f0;
      if (fh > sr * 0.48f) { amp[h] = 0.0f; continue; }
      float exactBin;

#if TC_PEAK_WIDE
      int rad = (int)fminf(0.035f * fh / binHz, 0.45f * out.f0 / binHz);
      if (rad < 2)  rad = 2;
      if (rad > 12) rad = 12;
#else
      int rad = 2;
#endif
      float m = peakAt(fh / binHz, nBins, &exactBin, rad);

      amp[h]  = (m > 2.5f * noiseFloor) ? m * winGain : 0.0f;

      {
        int c = (int)(exactBin + 0.5f);
        int b0 = c - halfW, b1 = c + halfW;
        if (b0 < 0) b0 = 0;
        if (b1 > nBins - 1) b1 = nBins - 1;
        for (int b = b0; b <= b1; b++) harmEnergy += gMag[b] * gMag[b];
      }

      if (h >= 3 && h < 12 && m > 1e-5f) {
        const double n2 = (double)(h + 1) * (h + 1);
        const double ratio = (double)(exactBin * binHz) / fh;
        const double y = ratio * ratio - 1.0;
        if (y > -0.30 && y < 0.60) {
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

    if (f >= shimA && f <= shimB && shimN < SHIM_MAX) {
      for (int hh = 0; hh < SHIM_HARM; hh++) gShimTrack[hh][shimN] = amp[hh];
      shimN++;
    }

    float tSec = (f - onset) * frameSec;
    int k = (int)(tc_timeWarp(tSec, out.noteDur) * TC_N_KEYFRAME);
    if (k < 0) k = 0;
    if (k > TC_N_KEYFRAME - 1) k = TC_N_KEYFRAME - 1;
    for (int h = 0; h < TC_N_HARM; h++) gHarmAcc[k][h] += amp[h];
    gLoudAcc[k] += gRms[f];
    gCnt[k]     += 1.0f;

    if (f >= susA && f <= susB) {
      for (int i = 0; i < nBins; i++) gAvgMag[i] += gMag[i];
      avgN++;
    }

    if (csv || trainSet) {
      float sum = 0.0f;
      for (int h = 0; h < TC_N_HARM; h++) sum += amp[h];
      if (sum > 1e-9f) {
        float normAmp[TC_N_HARM];
        for (int h = 0; h < TC_N_HARM; h++) normAmp[h] = amp[h] / sum;
        float noiseFrac2 = noiseFrac;

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
          in[2] = tc_clampf(tNorm, 0.0f, 1.5f);
          in[3] = (f >= relStart) ? 1.0f : 0.0f;
          trainSet->add(in, normAmp, noiseFrac2);
        }
      }
    }
  }
  if (csv) csv.close();

  {
    out.inharmonicity = 0.0f;
    const double N = inhN;
    if (N >= 8.0) {
      const double Sxx = inhSxx - inhSx * inhSx / N;
      const double Sxy = inhSxy - inhSx * inhSy / N;
      const double Syy = inhSyy - inhSy * inhSy / N;
      if (Sxx > 1e-9) {
        const double B   = Sxy / Sxx;
        const double sse = Syy - B * Sxy;
        const double se  = sqrt(fmax(sse, 0.0) / ((N - 2.0) * Sxx));

        const bool sig = (se <= 0.0) || (B > 2.0 * se);
        if (sig && B > 0.0) out.inharmonicity = tc_clampf((float)B, 0.0f, 0.002f);
        Serial.printf("[ANA] 非諧性擬合：B = %.6f ± %.6f（n=%d）%s\n",
                      B, se, (int)N, out.inharmonicity > 0.0f ? "" : " -> 不顯著，判 0");
      }
    }
  }
  (void)noiseAcc; (void)atkNoiseAcc; (void)atkNoiseN;

  if (shimN >= 24) {
    const int W = 9;
    float acc = 0.0f;
    int   accN = 0;
    for (int h = 0; h < SHIM_HARM; h++) {

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

        float ma = 0.0f;
        int   mn = 0;
        for (int j = i - W / 2; j <= i + W / 2; j++) {
          if (gShimTrack[h][j] < 1e-8f) continue;
          ma += logf(gShimTrack[h][j]); mn++;
        }
        if (mn < W - 2) continue;
        ma = expf(ma / mn);
        if (ma < 1e-8f) continue;
        float r = v / ma;
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

  if (!decaying) {

    float tmp[TC_N_KEYFRAME];
    int   n = 0;
    for (int k = 0; k < TC_N_KEYFRAME; k++)
      if (out.loud[k] > 0.01f) tmp[n++] = out.loud[k];
    float med = 0.7f;
    if (n >= 4) { qsort(tmp, n, sizeof(float), cmpf); med = tmp[n / 2]; }

    int kAtk = 0;
    while (kAtk < TC_N_KEYFRAME - 1 && out.loud[kAtk] < 0.9f * med) kAtk++;
    for (int k = kAtk; k < TC_N_KEYFRAME; k++) out.loud[k] = med;

    Serial.printf("[ANA] 持續型：起音佔 %d/%d 格，之後壓平到 %.2f（移除演奏者的漸強）\n",
                  kAtk, TC_N_KEYFRAME, med);
  }

  if (avgN > 0) {
    for (int i = 0; i < nBins; i++) gAvgMag[i] /= avgN;

    const float lo = logf(TC_SPECENV_FMIN), hi = logf(TC_SPECENV_FMAX);
    float envMax = -300.0f;
    for (int p = 0; p < TC_SPECENV_PTS; p++) {
      float fc  = expf(lo + (hi - lo) * p / (TC_SPECENV_PTS - 1));

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
      out.specEnv[p] = tc_clampf(out.specEnv[p] - envMax, -72.0f, 0.0f);
  }

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

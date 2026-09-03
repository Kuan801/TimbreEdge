#include "analyzer.h"
#include "wav_io.h"
#include "trainer.h"
#include <SD.h>

// ============================================================================
//  自帶的 radix-2 複數 FFT（不依賴 CMSIS 版本差異，2048 點約 0.2 ms）
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

// 最近一次分析在錄音裡數到幾次起音。1 = 正常的單音。
//
// 不放進 InstrumentProfile：那個結構會存進 SD，加欄位就得動 TC_PROFILE_MAGIC，
// 使用者既有的 BANK.BIN / PROFILE.BIN 全部作廢。這只是一個「剛剛那次分析」的
// 附帶結果，用不著持久化。
static int gOnsetCount = 1;
int analyzerLastOnsetCount() { return gOnsetCount; }

// 錄音品質的三個數字。分析反正要掃過整個檔案，順手量出來幾乎不花時間，
// 但少了它們就沒辦法回答「這次錄音到底行不行」——
// 而那正是站在機器前面的人唯一想知道的事。
static float gPeakAbs    = 0.0f;    // 全檔絕對峰值 0..1
static float gClipRatio  = 0.0f;    // 削波樣本佔比
static float gNoiseFloor = 0.0f;    // 起音之前的平均 RMS（相對峰值）
float analyzerLastPeak()       { return gPeakAbs;    }
float analyzerLastClipRatio()  { return gClipRatio;  }
float analyzerLastNoiseFloor() { return gNoiseFloor; }
DMAMEM static float  gYin[NFFT / 2];
DMAMEM static float  gHarmAcc[TC_N_KEYFRAME][TC_N_HARM];
DMAMEM static float  gLoudAcc[TC_N_KEYFRAME];
DMAMEM static float  gCnt[TC_N_KEYFRAME];
DMAMEM static float  gBuf[NFFT];

// 起音精細分析用：外差解調後的逐諧波包絡（103 格 × 2.9 ms）
#define ATK_FRAMES  ((int)(TC_ATK_WINDOW_SEC * TC_SAMPLE_RATE / TC_ATK_HOP))
DMAMEM static float  gAtkEnv[TC_N_HARM][ATK_FRAMES];
DMAMEM static float  gAtkTot[ATK_FRAMES];      // 同一時間軸上的「總能量」

// 持續段逐諧波的振幅軌跡，用來量 shimmer。只追前 12 根就夠代表。
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

// 就地 FFT，gRe/gIm 進、gRe/gIm 出
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
//  YIN 基頻偵測（輸入 gBuf 前 NFFT 點）
//  回傳 Hz，失敗回 0
// ============================================================================
static float yinPitch(const float *x, float sr) {
  const int W      = NFFT / 2;                       // 1024
  const int tauMin = (int)(sr / TC_F0_MAX);          // ~29
  const int tauMax = (int)(sr / TC_F0_MIN);          // ~678
  if (tauMax >= W) return 0.0f;

  // 1) 差分函數
  gYin[0] = 1.0f;
  for (int tau = 1; tau <= tauMax; tau++) {
    float s = 0.0f;
    for (int j = 0; j < W; j++) { float d = x[j] - x[j + tau]; s += d * d; }
    gYin[tau] = s;
  }
  // 2) 累積平均正規化
  float run = 0.0f;
  for (int tau = 1; tau <= tauMax; tau++) {
    run += gYin[tau];
    gYin[tau] = (run > 1e-12f) ? gYin[tau] * tau / run : 1.0f;
  }
  // 3) 絕對門檻 -> 第一個低於 threshold 的局部極小
  int best = -1;
  for (int tau = tauMin; tau < tauMax; tau++) {
    if (gYin[tau] < TC_YIN_THRESH) {
      while (tau + 1 < tauMax && gYin[tau + 1] < gYin[tau]) tau++;
      best = tau;
      break;
    }
  }
  if (best < 0) {                                     // 退回全域最小
    float m = 1e30f;
    for (int tau = tauMin; tau < tauMax; tau++)
      if (gYin[tau] < m) { m = gYin[tau]; best = tau; }
    if (best < 0 || m > 0.6f) return 0.0f;            // 太不週期，判定為噪音
  }
  // 4) 拋物線內插
  float betterTau = (float)best;
  if (best > 0 && best < tauMax - 1) {
    float a = gYin[best - 1], b = gYin[best], c = gYin[best + 1];
    float den = 2.0f * (2.0f * b - a - c);
    if (fabsf(den) > 1e-9f) betterTau = best + (c - a) / den;
  }
  return sr / betterTau;
}

// ============================================================================
//  起音精細分析：外差解調
//
//  FFT 的時間解析度受限於視窗長度（2048 點 = 46 ms），根本看不出「高次諧波
//  晚 20 ms 進來」這種差異。改用外差：把訊號乘上 exp(-j2πf_h t) 之後低通，
//  就得到該諧波的複數包絡，時間解析度只受低通的時間常數限制（這裡約 3 ms）。
//
//  成本：32 諧波 × 13230 取樣 × 約 12 ops ≈ 5 M ops，在 M7 上約 20 ms。
// ============================================================================
//  以「取樣為外層、諧波為內層」串流處理，所以不需要把整段起音存在記憶體裡，
//  只保留每根諧波的 4 個狀態變數（相位 + 低通）。
static void heterodyneAttack(WavReader &wav, uint32_t startSample,
                             float f0, float sr, int nHarm) {
  const float lpA = 1.0f - expf(-2.0f * (float)M_PI * TC_ATK_LP_HZ / sr);
  const int   nSamp = (int)(TC_ATK_WINDOW_SEC * sr);

  static float pr[TC_N_HARM], pi[TC_N_HARM];    // 旋轉相位
  static float lr[TC_N_HARM], li[TC_N_HARM];    // 低通狀態
  static float cw[TC_N_HARM], sw[TC_N_HARM];    // 每步的旋轉量
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

  float totLp = 0.0f;                           // 總能量（同一個低通，時間軸才對得上）
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
      if ((n & 255) == 255) {                   // 週期性正規化，避免數值漂移
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
//  非諧波（噪聲）比例：週期性殘差法
//
//  週期為 T 的訊號滿足 x[n] ≈ x[n-T]，所以 x[n] − x[n−T] 會把週期成分抵銷掉，
//  剩下的就是非週期成分（弓噪、氣聲、擊弦雜音）。對不相關的噪聲而言相減會
//  讓功率變兩倍，因此 噪聲比 = E(diff) / (2·E(x))。
//
//  為什麼不用前一版的「外差殘差」：要分辨 2.9 ms 的瞬態需要約 350 Hz 頻寬，
//  但要分開間隔 220 Hz 的諧波卻只能用 110 Hz —— 時頻不確定性擺在那裡。
//  結果是各諧波的偵測帶互相重疊、把整個頻譜都算成諧波，殘差恆為 0
//  （實測素材真值 35%，卻讀成 0.0%）。週期性殘差沒有這個矛盾：
//  它在時域運作，解析度只受窗長限制。
//
//  回傳 0..1；T 用線性內插支援非整數週期。
//
//  ★ 週期長度要現找，不能直接用整段的 f0（TC-GUITAR）
//
//  撥弦樂器的音高在撥下去之後會漂：實測這批吉他素材，分析器自己就報過
//  「音高擺動 48~57 cents，但沒有 3~9 Hz 的週期性」（所以不是顫音，是漂移）。
//  用整段平均的 f0 去算 x[n]-x[n-T]，T 差 50 cents（約 3%）就足以讓兩個週期
//  對不齊，相減之後剩下的其實是「相位對不上」而不是雜訊。
//
//  實測後果：吉他的持續段噪聲比被高估 2~13 倍（D3 量到 0.8%，用同一段訊號
//  以 evaluate.py 的方法量只有 0.06%）。合成器忠實地照這個數字送出寬頻噪聲，
//  於是每個音都掛著一層真實吉他沒有的嘶聲 —— 噪聲量誤差 +12 dB，
//  而 LSD、頻譜圖、質心四個指標全都看不到它（它們本來就對 -70 dB 的東西無感）。
//
//  作法：在 ±3% 內掃描週期長度，取殘差最小的那一個。定義上量的是
//  「用附近任何一個週期都解釋不掉的能量」，這才是非週期成分的本意。
//  對真的有寬頻噪聲的樂器（長笛氣聲、提琴弓噪）幾乎沒有影響 ——
//  換一個 T 也解釋不掉白噪聲，最小值跟原本的值一樣。
static float periodicNoiseRatio(WavReader &wav, uint32_t startSample,
                                float f0, float sr, float windowSec) {
  const float Tf0 = sr / f0;
  const int   nWin = (int)(windowSec * sr);
  const int   need = nWin + (int)(Tf0 * 1.03f) + 2;
  if (need > NFFT) return 0.0f;                 // gBuf 放不下就放棄

  if (wav.readMono(startSample, gBuf, need) < (uint32_t)need) return 0.0f;

  float best = 1.0f;
  // 7 個候選、±3%：吉他量到的漂移是 ±3%，再密下去改善不到 0.1 dB，
  // 而這段在 Teensy 上每個音要跑 6 次（起音 1 + 持續段 5）
  for (int k = -3; k <= 3; k++) {
    const float Tf   = Tf0 * (1.0f + 0.01f * k);
    const int   Ti   = (int)Tf;
    const float frac = Tf - Ti;
    if (Ti < 2 || Ti + 1 + nWin > need) continue;

    // 逐「週期」比對，而且要先把前一個週期的音量對齊到目前這個週期。
    //
    // 沒對齊的話，起音處會把「包絡正在快速上升」整個算成噪聲：鋼琴的振幅在
    // 一個週期(3.8 ms)內就能翻倍，x[n]-x[n-T] 大半來自音量變化而不是雜訊。
    // 早期版本把這個被高估的值當「振幅」用，數字剛好小所以沒出事；改成正確的
    // 能量->振幅換算(開根號)之後，誤差被放大成每個音頭一團寬頻爆音，
    // 鋼琴的質心相關性因此從 0.968 掉到 0.841。
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
      const float g = (float)sqrt(ea / eb);          // 把上一個週期的音量對齊過來
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

// 殘差（非週期成分）落在高頻的比例。
//
// 為什麼需要：長笛的氣聲和提琴的弓噪雖然「份量」差不多，落點卻完全不同 ——
// 實測真實素材，殘差在 2~5 kHz 的佔比：長笛 17.8%、提琴 65.6%。
// 用同一種頻譜形狀去合成，提琴會少掉那層沙沙的擦弦聲。
// 回傳「5*f0 以上佔殘差總能量的比例」，合成端據此決定要有多少走寬頻噪聲層。
static float noiseHighFraction(WavReader &wav, uint32_t startSample,
                               float f0, float sr) {
  const int Ti = (int)(sr / f0 + 0.5f);
  const int nWin = NFFT - Ti - 2;
  if (nWin < 1024) return 0.5f;
  if (wav.readMono(startSample, gBuf, NFFT) < (uint32_t)NFFT) return 0.5f;

  // 殘差補零到 NFFT 後做既有的定長 FFT（gRe/gIm 進、gRe/gIm 出）
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

// 回傳該諧波抵達自身峰值 50% 的時刻（秒）。抓不到回傳 -1。
//
// 要求「連續 3 格」都超過門檻才算數：起音瞬間的寬頻爆點（弓噪/擊弦雜音）
// 會讓每一根諧波的解調器同時被激發一下，若只看第一次越過門檻，所有諧波
// 都會被判定為 t=0，非同步性就完全量不出來。
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

// 在 targetBin 附近找峰並拋物線內插，回傳振幅、由 outBin 帶回精確位置
// rad = 往左右各找幾個 bin。預設 2 是給「理論位置就差不多是實際位置」的情況；
// 有非諧性的弦樂器高次諧波會跑掉好幾個 bin，窗太窄會停在窗邊，量到的位移
// 被截斷 —— 那正是舊版對鋼琴「剛好」量出合理 B 的原因（兩個錯誤互相抵銷）。
// 呼叫端要自己算 rad，上限必須小於相鄰諧波間距的一半，否則會抓到隔壁那根。
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
  return b - 0.25f * (a - cc) * d;                    // 內插後的峰值
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

  // ---------------------------------------------------------- 1) RMS 包絡 --
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
      // 0.997 而不是 1.0：int16 的滿刻度是 32767，而削波多半是在類比端就發生的，
      // 到達 ADC 時已經是一整段平頂，最高點未必剛好踩在 32767 上。
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
  for (int f = 0; f < nFrames; f++) gRms[f] /= rmsMax;   // 正規化 0..1

  // onset / peak / offset
  int onset = 0;
  while (onset < nFrames && gRms[onset] < 0.08f) onset++;
  int offset = nFrames - 1;
  while (offset > onset && gRms[offset] < 0.04f) offset--;
  if (offset - onset < 4) { Serial.println(F("[ANA] 有效音長太短")); wav.close(); return false; }

  int peakIdx = onset;
  for (int f = onset; f <= offset; f++) if (gRms[f] > gRms[peakIdx]) peakIdx = f;

  // 起音之前那段就是本次錄音的底噪。實測手機喇叭放給麥克風收的那一份，
  // 觸發整整早了 1.7 秒 —— 觸發的是 30~190 Hz 的環境低頻，訊噪比只有 7 dB，
  // 真正的音只佔最後 0.3 秒。這個數字看得出來，peak 看不出來。
  if (onset > 0) {
    float s = 0.0f;
    for (int f = 0; f < onset; f++) s += gRms[f];
    gNoiseFloor = s / onset;                       // gRms 已正規化到 0..1
  }

  const float frameSec = TC_HOP / sr;
  out.noteDur = (offset - onset) * frameSec;

  // ---------------------------------------------------------------------------
  //  這段錄音裡是不是不只一個音？
  //
  //  後面所有的量測都建立在「一個持續的單音」上：ADSR、衰減速率、shimmer、
  //  持續段的噪聲比，全部假設起音只有一次。錄了連續撥弦就會全盤失效 ——
  //  而且是無聲失效，照樣產生一個看起來正常的 profile。
  //
  //  實測（吉他，2 秒的麥克風錄音，每 0.30 秒撥一次共 6~7 下）：
  //      衰減 0.950（真值約 0.5）   每次新的撥弦都把音量拉回去，回歸出來接近不衰減
  //      shimmer 20%（真值 0%）     重複的起音被當成劇烈的振幅起伏，撞到上限
  //      起音 131~264 ms（真值 ~30） 起音偵測被後面幾下干擾
  //      噪聲比 0.258（真值 0.012）  持續段視窗裡夾著好幾個起音瞬態
  //  合成出來的吉他因此會像管風琴一樣一直響，而且抖得很厲害。
  //
  //  吉他這類衰減快的樂器最容易中招：錄 2 秒會很自然地一直撥。
  //
  //  判準：音量在一格之內跳升超過 6 dB，且跳完的位置高於峰值的 15%，
  //  兩次之間至少隔 150 ms。門檻是照實測資料訂的 ——
  //  真正的單音素材（原音檔、鋼琴、提琴、長笛、小號）都只會數到 1。
  // ---------------------------------------------------------------------------
  //  判準不能只看「音量跳升」—— 起音本身的斜坡就在跳升，實測 68 個已知單音
  //  素材有 35 個被誤判。真正的重新撥弦是「**先掉下去、再彈回來**」，
  //  所以要求：從上次起音以來的最低點算起，回升到 3 倍以上。
  //
  //  而且只看主峰之後：主峰之前本來就是起音，不可能有第二次。
  {
    const int minGap = (int)(0.15f / frameSec + 0.5f);
    int   count = 0, last = -9999;
    float runMin = 1e9f;                      // 上次起音以來的最低點
    for (int f = peakIdx + 1; f <= offset; f++) {
      if (gRms[f] < runMin) runMin = gRms[f];
      if (f - last < minGap) continue;
      if (gRms[f] < 0.25f)  continue;         // 太小聲的起伏不算（gRms 已正規化）
      if (runMin > 1e-4f && gRms[f] / runMin >= 3.0f) {
        count++; last = f; runMin = gRms[f];
      }
    }
    // 主峰那一下本身也算一次
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

  // ---------------------------------------------------------- 2) ADSR 擬合 --
  out.attack = fmaxf((peakIdx - onset) * frameSec, 0.003f);

  // ---- 「本體」：包絡維持在峰值 60% 以上的那一段 -------------------------
  //
  // 舊版把 sustain 視窗定成「峰值之後的 35%~75%」，這在真實素材上會出大問題：
  // 一段 2.6 秒的小提琴，那個視窗剛好落在「運弓結束、聲音正在收掉」的地方
  // （實測包絡 0.61 -> 0.12），於是小提琴被判成「衰減型樂器」，
  // 拿到和鋼琴一模一樣的包絡行為 —— 兩種樂器最大的聽覺差異就這樣被抹平。
  //
  // 改用結構性的判準：本體佔音長的比例。
  //   鋼琴  本體 0.19 s / 音長 3.25 s =  6%   -> 衰減型
  //   小提琴 本體 1.20 s / 音長 1.70 s = 71%   -> 持續型
  // 這個量測跟「衰減速率」無關，不會被素材長度或運弓收尾誤導。
  int bodyA = onset, bodyB = offset;
  {
    int f = onset;
    while (f <= offset && gRms[f] < 0.6f) f++;
    if (f <= offset) bodyA = f;
    f = offset;
    while (f > bodyA && gRms[f] < 0.6f) f--;
    bodyB = f;
  }
  // 本體短到量不出來，代表衰減極快（鋼琴的某些音就是這樣，>60% 只維持兩格）。
  // 這時要判定成「衰減型」，並且只取峰值附近一小段當作頻譜量測視窗。
  //
  // 早期版本這裡的 fallback 是 bodyB = (peakIdx + offset)/2 —— 方向完全相反，
  // 反而給了一個超長的假本體，把鋼琴 Bb4 判成持續型（本體 49%），
  // 包絡因此被壓平、變成不會衰減的長音。
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

  // 峰值位置也要看：撥弦/擊弦的能量在起音那一下就給完了，之後只會變小，
  // 物理上不可能「越拉越大聲」。所以峰值落在後半段就一定不是衰減型。
  //
  // 這條在真實素材上很關鍵：小提琴常常是漸強（實測 12 個素材全部都是，
  // 包絡從 0.3 一路升到 1.0），本體因此又晚又短，光看 bodyFrac 會把
  // 弓弦誤判成撥弦 —— 實測 G4 的本體只佔 15%。
  const float peakPos = (float)(peakIdx - onset) / (float)(offset - onset + 1);
  const bool  decaying = (bodyFrac < 0.25f) && (peakPos < 0.25f);

  // 頻譜包絡平均、shimmer、噪聲量測全部改用本體區間
  int susA = bodyA, susB = bodyB;

  // ---- shimmer 專用的取樣窗 ----
  //
  // shimmer 不該被綁在「本體」上。上面那段註解自己就寫了：小提琴幾乎都是漸強，
  // 本體因此又晚又短。這批素材的高音更誇張 —— 實測 Ab5 的本體只佔音長 18%
  // （0.92~1.13 s，19 格）、B5 佔 21%（23 格），而 shimmer 至少要 24 格才肯算，
  // 於是這兩個音的 shimmer 被判成 0。B5 只差一格。
  //
  // 而 shimmer 量的是「除以 9 格（104 ms）局部移動平均之後剩下的抖動」，
  // 那個去趨勢本來就會把漸強、漸弱、任何比視窗慢的形狀完全消掉。
  // 所以限制在本體內對量測本身沒有幫助，只是白白少掉可用的格數。
  //
  // 只對持續型放寬。衰減型（鋼琴/撥弦）維持原樣：它們的本體判定是對的，
  // 而且把窗往前拉會吃進起音後那段極快的衰減 —— 當初 shimmer 改寫成對數域
  // 移動平均，正是為了修掉鋼琴被誤量成 22.6% 的假抖動，不能倒退回去。
  // 只往前延伸起點，終點一定維持在本體結束。
  //
  // 試過把終點也放到 offset：shimN 從 19 變成 89，可是 accN 反而垮掉
  // （B3 從 9 掉到 1，連本來量得好好的音都壞了）。原因是下面那道「衰減太快
  // 就不列入」的閘門會拿窗尾的振幅跟窗頭比，一旦把收弓的 release 吃進來，
  // last/first 幾乎一定小於 0.25，於是每根諧波都被判成衰減、全數剔除。
  // 本體的終點本來就是「收弓開始」的位置，那個判定是對的，不要動它。
  int shimA = susA, shimB = susB;
  if (!decaying) {
    const int a = onset + (int)(TC_SHIM_START_SEC / frameSec);  // 跳過起音暫態（比 MA 視窗快，去不掉）
    if (a < shimA) shimA = a;
  }
  {
    static float tmp[TC_MAX_FRAMES];
    int n = 0;
    for (int f = susA; f <= susB && n < TC_MAX_FRAMES; f++) tmp[n++] = gRms[f];
    qsort(tmp, n, sizeof(float), cmpf);
    out.sustain = tc_clampf(tmp[n / 2], 0.02f, 1.0f);
  }
  // decay = 本體開始後掉到 sustain*1.05 所需時間（持續型才用得到）
  {
    int f = bodyA;
    while (f < bodyB && gRms[f] > out.sustain * 1.05f) f++;
    out.decay = fmaxf((f - bodyA) * frameSec, 0.01f);
  }

  // release：
  //   持續型 -> 就是本體結束後聲音收掉的那段（運弓/吹氣停止）
  //   衰減型 -> 那是制音器的時間，很短；長長的自然衰減由 sustainDecayPerSec 負責，
  //             不能把它算進 release，否則鋼琴的 release 會被算成好幾秒
  out.release = decaying ? 0.20f
                         : fmaxf((offset - bodyB) * frameSec, 0.10f);

  // 持續段的自然衰減：用對數域線性回歸，比取兩端點穩健得多
  {
    int rA = bodyA;
    int rB = decaying ? offset : bodyB;      // 衰減型要看長一點才量得到真正的衰減率
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
    // 下限原本是 0.15。實測撥弦高音的真實值就落在 0.130~0.179（獨立用包絡回歸
    // 量的），也就是下限一直在作用，把不同的音全部壓成一模一樣的 0.150 ——
    // 顯示出來會誤導，「換樂器」的音色距離也少了一項有效資訊。
    //
    // 放寬到 0.05（每秒掉到 5%，約 -26 dB/秒）。再低就不像樂器而像雜訊突波了。
    //
    // 注意：這個值不影響合成。實際播放用的是 loud[32] 那條實測包絡曲線
    // （見 profile.h 的說明），這裡只供顯示與 profileTimbreDistance 使用。
    if (decaying) perSec = tc_clampf(perSec, 0.05f, 0.97f);
    else          perSec = tc_clampf(perSec, 0.95f, 1.0f);
    out.sustainDecayPerSec = (perSec > 0.97f) ? 1.0f : perSec;
  }

  out.envHoldNorm = 1.0f;   // 持續型的曲線稍後會被壓平，不需要提前停住

  Serial.printf("[ANA] 本體 %.2f~%.2f s（佔音長 %.0f%%）-> 判定為%s\n",
                (bodyA - onset) * frameSec, (bodyB - onset) * frameSec,
                bodyFrac * 100.0f, decaying ? "衰減型" : "持續型");

  // ---------------------------------------------------- 3) YIN 取基頻中位數 --
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

  // ------------------------------------------------ 3a) 顫音深度 -----------
  //
  // 舊版用「9 個 YIN 候選的離散度」當顫音深度。那是錯的量：離散度會同時
  // 收進演奏者的緩慢音高漂移、YIN 自己的估計誤差、還有偶發的八度誤判。
  // 實測標示 nonvib（無顫音）的長笛素材，舊版量出 2.3~11.5 cents 的顫音，
  // 合成時就被加上一層真實素材沒有的擺動。
  //
  // 顫音在物理上是「4~8 Hz 的週期性音高調變」，所以要量的是週期性，不是離散度：
  //   1) 在 f0 做外差解調，取出瞬時頻率軌跡（172 Hz 取樣，最長 1.5 秒）
  //   2) 扣掉 0.5 秒移動平均 —— 把緩慢漂移拿掉，只留下擺動
  //   3) 在 3~9 Hz 掃 DFT 找峰值；只有當這個頻帶佔了殘差變異數夠大的比例，
  //      才認定是真的顫音，否則判為 0（那只是估計噪聲）
  {
    // 解調的累加窗長要取「f0 週期的整數倍」。
    // 方形窗的頻率響應在 sr/DECIM 的整數倍處為零，窗長對齊 f0 週期時，
    // 所有諧波(n*f0)的偏移量就正好落在零點上，等於一組完美的梳狀陷波。
    // 沒對齊的話第二諧波會洩漏進基帶 —— 長笛 A4 的 h2 比基頻還強，
    // 用 DECIM=256 時洩漏 -18 dB，量到的音高擺動本底高達 35 cents，
    // 連植入 30 cents 的顫音都會被蓋掉（做過陽性對照確認）。
    int m = (int)(out.f0 / 130.0f + 0.5f);        // 讓解調後取樣率落在 130 Hz 上下
    if (m < 1) m = 1;
    const int   DECIM = (int)(m * sr / out.f0 + 0.5f);   // 不能叫 DEC：Print.h 有 #define DEC 10
    const int   MAXP = 256;
    const float fsD   = sr / (float)DECIM;
    static float trk[MAXP];
    int np = 0;

    const float w  = 2.0f * (float)M_PI * out.f0 / sr;
    const float cw = cosf(w), sw = sinf(w);
    float cr = 1.0f, ci = 0.0f;                   // 遞迴振盪器，比每點呼叫 sinf 快
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
        // 旋轉 + 週期性正規化（浮點遞迴會慢慢偏離單位圓）
        float nr = cr * cw - ci * sw;
        ci = cr * sw + ci * cw;
        cr = nr;
        if (((i & 1023) == 1023)) {
          float g = 1.5f - 0.5f * (cr * cr + ci * ci);
          cr *= g; ci *= g;
        }
        if (++cnt >= DECIM) {
          if (havePrev) {
            // z[k] * conj(z[k-1]) 的輻角 = 這段期間的平均相位增量
            float re = accI * pI + accQ * pQ;
            float im = accQ * pI - accI * pQ;
            float dphi = atan2f(im, re);
            trk[np++] = 1731.2f * (dphi * fsD / 6.2831853f) / out.f0;   // 音分
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
      // 2) 扣掉 0.5 秒移動平均，去掉緩慢漂移
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

      // 3) 3~9 Hz 掃 DFT
      float best = 0.0f, bestHz = 0.0f;
      for (float fv = 3.0f; fv <= 9.01f; fv += 0.25f) {
        float re = 0.0f, im = 0.0f;
        float ph = 0.0f, dp = 2.0f * (float)M_PI * fv / fsD;
        for (int i = 0; i < np; i++) { re += det[i] * cosf(ph); im -= det[i] * sinf(ph); ph += dp; }
        float mag = 2.0f * sqrtf(re * re + im * im) / (float)np;   // 正弦振幅
        if (mag > best) { best = mag; bestHz = fv; }
      }
      // 這個頻率成分的功率佔殘差變異數的比例；真顫音會很集中
      float share = (tot > 1e-9f) ? (0.5f * best * best) / tot : 0.0f;
      if (share > 0.25f && best > 2.0f) {
        // 0.5 秒的移動平均本身也會削掉一部分顫音（衰減量 = |sinc(f * 0.5s)|），
        // 把它除回去。陽性對照：植入 12/30 cents，未補償時量到 11.2/26.6。
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

  // -------------------------------- 3b) 起音精細分析（外差解調）------------
  // FFT 看不出「高次諧波晚 20 ms 才進來」，但這正是人耳辨識樂器最主要的線索。
  heterodyneAttack(wav, (uint32_t)onset * TC_HOP, out.f0, sr, TC_N_HARM);

  {
    float t1 = onsetTimeOf(0);
    if (t1 < 0.0f) t1 = 0.0f;
    for (int h = 0; h < TC_N_HARM; h++) {
      float th = onsetTimeOf(h);
      out.harmOnset[h] = (th < 0.0f) ? 0.0f : tc_clampf(th - t1, 0.0f, 0.15f);
    }
    // 3 點中位數濾波：單根諧波偶爾會被雜訊誤判，濾掉離群值後
    // 起音的整體走向才聽得出來（而不是變成隨機抖動）
    {
      float tmp[TC_N_HARM];
      for (int h = 0; h < TC_N_HARM; h++) {
        float a = out.harmOnset[h > 0 ? h - 1 : 0];
        float b = out.harmOnset[h];
        float c = out.harmOnset[h < TC_N_HARM - 1 ? h + 1 : TC_N_HARM - 1];
        float mx = fmaxf(a, fmaxf(b, c)), mn = fminf(a, fminf(b, c));
        tmp[h] = a + b + c - mx - mn;                 // 中位數
      }
      for (int h = 0; h < TC_N_HARM; h++) out.harmOnset[h] = tmp[h];
    }
    // 再與「延遲隨諧波序號線性增加」的擬合結果各取一半。
    // 弱諧波的 50% 判定本來就不穩，量到的散佈遠大於真實值；
    // 混入趨勢線可以保住「高次諧波晚進來」這個感知線索，又不會變成亂跳。
    {
      float sx = 0, sy = 0, sxx = 0, sxy = 0;
      int   n = 0;
      for (int h = 0; h < TC_N_HARM; h++) {
        if (out.harmOnset[h] <= 0.0f && h > 0) continue;   // 沒量到的跳過
        sx += h; sy += out.harmOnset[h];
        sxx += (float)h * h; sxy += h * out.harmOnset[h];
        n++;
      }
      float slope = 0.0f;
      if (n >= 6) {
        float den = n * sxx - sx * sx;
        if (fabsf(den) > 1e-6f) slope = (n * sxy - sx * sy) / den;
      }
      if (slope < 0.0f) slope = 0.0f;                 // 只允許「越高次越晚」
      for (int h = 0; h < TC_N_HARM; h++) {
        float fit = slope * h;
        out.harmOnset[h] = tc_clampf(0.5f * out.harmOnset[h] + 0.5f * fit, 0.0f, 0.06f);
      }
    }
    // 用外差包絡重新估起音時間：比 11.6 ms 解析度的 RMS 準得多
    float tot[ATK_FRAMES];
    float mx = 0.0f;
    int   mxIdx = 0;
    for (int i = 0; i < ATK_FRAMES; i++) {
      tot[i] = 0.0f;
      for (int h = 0; h < TC_N_HARM; h++) tot[i] += gAtkEnv[h][i];
      if (tot[i] > mx) { mx = tot[i]; mxIdx = i; }
    }
    // 峰值落在視窗尾端代表起音比視窗還長，這時保留原本 RMS 的估計
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

  // -------------------------------- 3c) 起音瞬態（擊弦聲 / 弓噪 / 氣聲）----
  // 這是人耳辨識樂器最強的線索，也是這支程式最早漏掉的東西。
  //
  // 舊版用 FFT 逐格的「總能量 − 諧波能量」估算，但 FFT 視窗長 46 ms，
  // 一個 6 ms 的擊弦聲被稀釋到幾乎量不到（實測素材真值 35%，卻讀成 0.000）。
  // 改成直接用外差殘差：總能量減掉所有諧波的能量，剩下的就是非諧波成分，
  // 時間解析度 2.9 ms，短瞬態再也躲不掉。
  {
    out.attackNoise     = periodicNoiseRatio(wav, (uint32_t)onset * TC_HOP, out.f0, sr, 0.030f);
    // 起音噪聲的落點也要量。實測真實素材的起音殘差（C4）：
    //   鋼琴 93% 在 1.3 kHz 以下、3 kHz 以上是 0
    //   長笛 84% 在 1.3~3 kHz
    // 合成端原本一律用寫死的 9 kHz 寬頻，等於在每個鋼琴音頭噴一團它本來
    // 沒有的高頻嘶聲 —— 鋼琴的質心相關性因此從 0.968 掉到 0.848。
    out.attackHighFrac  = noiseHighFraction(wav, (uint32_t)onset * TC_HOP, out.f0, sr);
    // 持續段的噪聲也改用同一個方法量（原本 FFT 那套會被諧波帶寬吃掉）。
    //
    // 但不能只取一個 30 ms 視窗：30 ms 對鋼琴 C4 只有 8 個週期，估計值非常抖。
    // 實測逐音的噪聲份量誤差在 -5.0 ~ +7.1 dB 之間亂跳，質心相關性也跟著
    // 從 0.99 掉到 0.61。改成沿著持續段取 5 個點、拿中位數，離群值就不會主導。
    //
    // ★ 取樣視窗要落在「真的在持續」的那一段（TC-GUITAR）
    //
    // susA..susB 是「本體」，對衰減型樂器來說那是很短的一小段：實測這批
    // 吉他素材，D3 的本體是 0.00~0.46 秒，於是 5 個取樣點全部落在撥弦的
    // 餘波裡。那一段的殘差比本來就高（弦還沒穩定），量到 0.8%；同一個音
    // 在 0.4~1.6 秒之間只有 0.06% —— 差 13 倍。
    //
    // 合成器把這個數字當成「整個持續段的噪聲量」，於是每個吉他音從頭到尾
    // 都掛著一層真實吉他沒有的嘶聲。噪聲量誤差 +12 dB，而 LSD、頻譜圖、
    // 質心三個指標完全看不到（它們對 -70 dB 的東西本來就無感），
    // 只有 evaluate.py 的噪聲量欄位抓得到。
    //
    // 起音那一段本來就有 attackNoise 在負責（前 150 ms，30 ms 時間常數），
    // 拿它去代表持續段是重複計算。所以這裡把視窗往後推：
    //   起點：起音後 0.35 秒（跳過撥弦／擊弦的餘波）
    //   終點：電平掉到峰值 -25 dB 為止（再低下去量到的是錄音本底不是樂器，
    //         實測吉他 D3 在 -20 dB 之後殘差比又開始爬升）
    // 湊不出 0.2 秒就退回原本的本體視窗（短音、或整段都很小聲時）。
    //
    // 只對衰減型套用。持續型（長笛/小號/提琴）的本體就是持續段本身，
    // 視窗是對的，不要動 —— 實測把它們的視窗一起往後挪，噪聲落點誤差
    // 反而全部變差 0.3~1.0 個百分點（視窗尾端吃進了收弓/收氣那一段）。
    // 這不是「照樂器種類分支」：decaying 是從包絡量出來的（本體佔音長
    // < 25% 且峰值在前 25%），analyzer 別的地方（release、衰減率）
    // 早就用同一個量在分流。
    {
      int nsA = susA, nsB = susB;
      if (decaying) {
        const int a = onset + (int)(0.35f / frameSec);
        int b = (a > susB) ? a : susB;
        while (b + 1 <= offset && gRms[b + 1] > 0.056f) b++;    // -25 dB（gRms 已正規化）
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

  // ------------------------------------------- 4) 逐格 FFT 抽諧波 + 頻譜包絡 --
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

  // 尾端 20% 視為「已放開」，對應 MLP 的第 4 個輸入特徵
  const int   relStart  = onset + (int)((offset - onset) * 0.80f);
  const float binHz     = sr / NFFT;
  const int   nBins     = NFFT / 2;
  const float winGain   = 2.0f / (NFFT * 0.5f);        // Hann 相干增益補償
  float noiseAcc = 0.0f;
  // 非諧性用二參數最小平方法擬合（見下方 4b 的說明），這裡累積它需要的和。
  // 用 double：Sxx 會累到 10^8 量級，float 的 24 bit 尾數在這裡已經不夠。
  // 這是離線分析、每個音只跑一次，M7 上軟體模擬 double 的成本可以忽略。
  double inhN = 0.0, inhSx = 0.0, inhSy = 0.0, inhSxx = 0.0, inhSxy = 0.0, inhSyy = 0.0;
  int   avgN     = 0;
  float atkNoiseAcc = 0.0f;                 // 起音前 30 ms 的噪聲比
  int   atkNoiseN   = 0;
  const int atkNoiseEnd = onset + (int)(0.030f * sr / TC_HOP) + 1;
  int   shimN = 0;                          // shimmer 追蹤到第幾格

  for (int f = onset; f <= offset; f++) {
    uint32_t pos = (uint32_t)f * TC_HOP;
    if (pos + NFFT > N) break;
    wav.readMono(pos, gBuf, NFFT);

    // 每 16 格回報一次進度（回呼本身可能要畫 OLED，別叫太密）
    if (gProgressCb && ((f - onset) & 15) == 0)
      gProgressCb((float)(f - onset) / (float)(offset - onset + 1));

    for (int i = 0; i < NFFT; i++) { gRe[i] = gBuf[i] * gWin[i]; gIm[i] = 0.0f; }
    fft();
    float total = 0.0f;
    for (int i = 0; i < nBins; i++) {
      gMag[i] = sqrtf(gRe[i] * gRe[i] + gIm[i] * gIm[i]);
      total  += gMag[i] * gMag[i];
    }

    // ---- 噪聲底線：取「相鄰諧波正中間」那些 bin 的中位數 -------------------
    // 那些位置一定不是諧波，所以直接反映本底。沒有這道閘的話，埋在本底裡的
    // 高次諧波會被當成真的訊號，正規化後拿到 1~3% 的份量，
    // 合成出來就多了一層真實樂器沒有的高頻「毛邊」。
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

    // 諧波抽取
    float amp[TC_N_HARM];
    float harmEnergy = 0.0f;
    // 每根諧波佔用的 bin 半寬：不能超過相鄰諧波間距的一半，否則低音會重複計算
    int   halfW = (int)(out.f0 / binHz * 0.5f);
    if (halfW > 2) halfW = 2;
    if (halfW < 1) halfW = 1;

    for (int h = 0; h < TC_N_HARM; h++) {
      float fh = (h + 1) * out.f0;
      if (fh > sr * 0.48f) { amp[h] = 0.0f; continue; }
      float exactBin;
      // 搜尋半徑跟著「這根諧波可能跑多遠」走：
      //   3.5% 的 fh  -> 涵蓋 B 到 0.0005 為止的位移（鋼琴約 0.0003）
      //   0.45 * f0   -> 硬上限，不能碰到隔壁那根諧波
#if TC_PEAK_WIDE
      int rad = (int)fminf(0.035f * fh / binHz, 0.45f * out.f0 / binHz);
      if (rad < 2)  rad = 2;
      if (rad > 12) rad = 12;
#else
      int rad = 2;
#endif
      float m = peakAt(fh / binHz, nBins, &exactBin, rad);
      // 沒有明顯高過本底就當作沒有這根諧波
      amp[h]  = (m > 2.5f * noiseFloor) ? m * winGain : 0.0f;
      // 實際加總峰值周圍的能量，而不是用固定倍率近似。
      // 舊版用 m*m*3 常常高估，把 noiseFrac 壓成 0，噪聲層等於失效。
      {
        int c = (int)(exactBin + 0.5f);
        int b0 = c - halfW, b1 = c + halfW;
        if (b0 < 0) b0 = 0;
        if (b1 > nBins - 1) b1 = nBins - 1;
        for (int b = b0; b <= b1; b++) harmEnergy += gMag[b] * gMag[b];
      }

      // 非諧性：累積 (n^2, ratio^2 - 1) 這一對，最後一起擬合。
      // 為什麼不像舊版那樣「每根各算一個 B 再平均」—— 見下方 4b。
      if (h >= 3 && h < 12 && m > 1e-5f) {
        const double n2 = (double)(h + 1) * (h + 1);
        const double ratio = (double)(exactBin * binHz) / fh;
        const double y = ratio * ratio - 1.0;
        if (y > -0.30 && y < 0.60) {          // 明顯抓錯峰的就不要進迴歸
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

    // 持續段逐諧波軌跡（量 shimmer 用）
    if (f >= shimA && f <= shimB && shimN < SHIM_MAX) {
      for (int hh = 0; hh < SHIM_HARM; hh++) gShimTrack[hh][shimN] = amp[hh];
      shimN++;
    }

    // 累進到關鍵影格 —— 用對數時間軸，讓起音拿到足夠的格數
    float tSec = (f - onset) * frameSec;
    int k = (int)(tc_timeWarp(tSec, out.noteDur) * TC_N_KEYFRAME);
    if (k < 0) k = 0;
    if (k > TC_N_KEYFRAME - 1) k = TC_N_KEYFRAME - 1;
    for (int h = 0; h < TC_N_HARM; h++) gHarmAcc[k][h] += amp[h];
    gLoudAcc[k] += gRms[f];
    gCnt[k]     += 1.0f;

    // 平均頻譜（持續段）用來做頻譜包絡
    if (f >= susA && f <= susB) {
      for (int i = 0; i < nBins; i++) gAvgMag[i] += gMag[i];
      avgN++;
    }

    // ---- 訓練樣本（CSV 匯出與機上訓練共用同一份特徵/目標）----------------
    if (csv || trainSet) {
      float sum = 0.0f;
      for (int h = 0; h < TC_N_HARM; h++) sum += amp[h];
      if (sum > 1e-9f) {
        float normAmp[TC_N_HARM];
        for (int h = 0; h < TC_N_HARM; h++) normAmp[h] = amp[h] / sum;
        float noiseFrac2 = noiseFrac;
        // MLP 的時間特徵也要用同一套對數時間軸，否則訓練與推論對不上
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
          in[2] = tc_clampf(tNorm, 0.0f, 1.5f);   // 已是對數時間軸
          in[3] = (f >= relStart) ? 1.0f : 0.0f;
          trainSet->add(in, normAmp, noiseFrac2);
        }
      }
    }
  }
  if (csv) csv.close();

  // noiseGain / attackNoise 已在 3c) 用週期性殘差量好，這裡不覆蓋。

  // ------------------------------- 4b) 非諧性：把「音高估錯」跟真的非諧性分開 --
  //
  // 舊版對每根諧波各算一個 B = (ratio^2 - 1) / n^2 再取平均。這條式子把
  // 「f0 估偏」直接翻譯成非諧性：f0 若偏低 δ，每一根諧波看起來都偏高同樣的
  // 比例，於是 ratio^2 - 1 ≈ 2δ 對所有 n 都一樣，除以 n^2 之後仍然是正的，
  // 平均起來就變成一個假的正 B。而最後那道 clamp 下限是 0，負值被夾掉、
  // 正值留著 —— 偏差是單向的，只會高估、不會低估。
  //
  // 實測後果：Iowa 的小提琴素材（B 物理上應該 ~0）被量到 C#4 = 0.00027、
  // G4 = 0.00035，那是鋼琴的量級。B = 0.00035 時第 20 根諧波偏高 113 cents、
  // 第 32 根偏高 265 cents，三聲部一起響就是一片沙沙聲。
  //
  // 正確的作法是同時擬合兩個東西：
  //
  //     ratio^2 - 1  =  c  +  B * n^2
  //                     ^     ^
  //                     |     +-- 真的非諧性：偏移量隨 n^2 成長
  //                     +-------- f0 估偏：對所有 n 都一樣的常數
  //
  // 這是標準的一元線性迴歸（自變數 x = n^2）。c 把音高誤差整個吸收掉，
  // B 因此對 f0 的估計誤差免疫 —— 這正是舊版缺的那一項。
  //
  // 再加一道顯著性檢定：只有當 B 大於自身標準誤的 2 倍才採信，否則判 0。
  // 提琴/管樂的 B 本來就在雜訊裡，硬要報一個數字不如老實說沒有。
  {
    out.inharmonicity = 0.0f;
    const double N = inhN;
    if (N >= 8.0) {
      const double Sxx = inhSxx - inhSx * inhSx / N;      // 中心化平方和
      const double Sxy = inhSxy - inhSx * inhSy / N;
      const double Syy = inhSyy - inhSy * inhSy / N;
      if (Sxx > 1e-9) {
        const double B   = Sxy / Sxx;                      // 斜率 = 非諧性係數
        const double sse = Syy - B * Sxy;                  // 殘差平方和
        const double se  = sqrt(fmax(sse, 0.0) / ((N - 2.0) * Sxx));   // B 的標準誤
        // 顯著且為正才採信。se 為 0（完美擬合）時直接接受。
        const bool sig = (se <= 0.0) || (B > 2.0 * se);
        if (sig && B > 0.0) out.inharmonicity = tc_clampf((float)B, 0.0f, 0.002f);
        Serial.printf("[ANA] 非諧性擬合：B = %.6f ± %.6f（n=%d）%s\n",
                      B, se, (int)N, out.inharmonicity > 0.0f ? "" : " -> 不顯著，判 0");
      }
    }
  }
  (void)noiseAcc; (void)atkNoiseAcc; (void)atkNoiseN;

  // -------------------------------------------- shimmer（持續段微觀起伏）--
  // 要量的是「圍繞趨勢的快速抖動」，不是「音在衰減」。
  //
  // 舊版用單一指數擬合去趨勢，但鋼琴這種快速衰減的樂器，衰減本身根本不是
  // 單一指數（高次諧波掉得比基頻快好幾倍），殘差會被誤判成起伏 ——
  // 實測鋼琴被量成 22.6% shimmer，合成時就被加上巨大的假顫動，
  // 聽起來像合成器 pad，而且把鋼琴和提琴都homogenize成同一種聲音。
  //
  // 改成「除以局部移動平均」：不管衰減是什麼形狀都會被完全消掉，
  // 留下的純粹是比視窗更快的抖動。
  if (shimN >= 24) {
    const int W = 9;                            // 約 104 ms 的移動平均
    float acc = 0.0f;
    int   accN = 0;
    for (int h = 0; h < SHIM_HARM; h++) {
      // 衰減太快的諧波不列入：46 ms 的 FFT 視窗量一個 27/秒 的衰減時，
      // 視窗內振幅就掉了 70%，量到的「起伏」其實是量測誤差。
      // 鋼琴的高次諧波就是這樣被誤判成 shimmer 的。
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
        // 對數域平均：對「指數衰減」是零偏差的。
        // 用一般算術平均的話，高次諧波衰減得快（實測 18/秒），
        // 窗內的曲率會被當成起伏，鋼琴因此被量出 10.7% 的假 shimmer。
        float ma = 0.0f;
        int   mn = 0;
        for (int j = i - W / 2; j <= i + W / 2; j++) {
          if (gShimTrack[h][j] < 1e-8f) continue;
          ma += logf(gShimTrack[h][j]); mn++;
        }
        if (mn < W - 2) continue;
        ma = expf(ma / mn);
        if (ma < 1e-8f) continue;
        float r = v / ma;                       // 去趨勢後圍繞 1
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
    // 合格的諧波太少就代表「量不出來」——這時要老實給 0，
    // 不能拿一兩根雜訊很大的殘存諧波去代表整體，否則會憑空生出假顫動。
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

  // ------------------------------------------------- 5) 關鍵影格正規化 ------
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

  // ------------------- 5b) 持續型：把「演奏者的漸強」從包絡裡拿掉 ---------
  //
  // 錄音裡的音量起伏對弓弦/管樂來說是**演奏表情**，不是樂器的固有性質。
  // 實測 12 個小提琴素材全部都是漸強（0.3 -> 1.0），照著重播的話每個合成音
  // 都會自己漸強，完全不能用來演奏旋律。
  //
  // 所以持續型只保留「起音」那一段（那才是樂器特徵：運弓的建立時間、
  // 弓噪），之後一律壓平到本體的中位數。衰減型不動 —— 它的衰減就是聲音本身。
  if (!decaying) {
    // 本體中位數
    float tmp[TC_N_KEYFRAME];
    int   n = 0;
    for (int k = 0; k < TC_N_KEYFRAME; k++)
      if (out.loud[k] > 0.01f) tmp[n++] = out.loud[k];
    float med = 0.7f;
    if (n >= 4) { qsort(tmp, n, sizeof(float), cmpf); med = tmp[n / 2]; }

    // 起音結束 = 第一次到達 0.9 倍中位數的位置
    int kAtk = 0;
    while (kAtk < TC_N_KEYFRAME - 1 && out.loud[kAtk] < 0.9f * med) kAtk++;
    for (int k = kAtk; k < TC_N_KEYFRAME; k++) out.loud[k] = med;

    Serial.printf("[ANA] 持續型：起音佔 %d/%d 格，之後壓平到 %.2f（移除演奏者的漸強）\n",
                  kAtk, TC_N_KEYFRAME, med);
  }

  // -------------------------------------------------- 6) log 頻譜包絡 ------
  if (avgN > 0) {
    for (int i = 0; i < nBins; i++) gAvgMag[i] /= avgN;

    const float lo = logf(TC_SPECENV_FMIN), hi = logf(TC_SPECENV_FMAX);
    float envMax = -300.0f;
    for (int p = 0; p < TC_SPECENV_PTS; p++) {
      float fc  = expf(lo + (hi - lo) * p / (TC_SPECENV_PTS - 1));
      // 視窗寬度要有下限 0.6*f0（總寬 1.2*f0），否則低頻段的「包絡」其實
      // 還留著諧波的梳狀結構 —— ±12% 在 fc 小的時候比諧波間距窄很多，
      // 視窗會整個掉進兩根諧波之間的谷底。
      //
      // 實測小號 B4（f0=494.6）：查 698 Hz（h1 與 h2 正中間）得到 -55.8 dB，
      // 移調到 F5 時基頻的校正增益變成 0.004，被壓掉 48 dB ——
      // 合成出來的 F5 基頻只剩 1.0%，真值是 30%。
      // 這也解釋了為什麼誤差對移調距離「非單調」：八度落在諧波上還好，
      // 三全音落在谷底就崩掉。
      //
      // 下限取 0.6*f0 而不是更大，是因為再寬就會抹平高頻的細節
      // （實測 2.5k~5k 的滾降在 0.6 時保住 18.0 dB，0.8 以上就開始變形）。
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
      out.specEnv[p] = tc_clampf(out.specEnv[p] - envMax, -72.0f, 0.0f);   // 以峰值為 0 dB
  }

  // 亮度（頻譜質心 / f0），純粹給人看
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

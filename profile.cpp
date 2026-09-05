#include "profile.h"
#include <SD.h>

// ============================================================================
//  ProfileBank
// ============================================================================
// ---------------------------------------------------------------------------
//  The "this doesn't look like the same instrument" check, run before storing
//
//  We compare against "the closest entry in the bank", not the average ——
//  synthesis already picks the entry with the nearest pitch, and averaging gets
//  wrecked by the piano (a piano's spread within one instrument is wider than
//  "trumpet vs violin", since every note has its own string and hammer).
//
//  Say nothing while the bank holds fewer than TC_TIMBRE_WARN_MIN_REFS entries:
//  measured false-positive rate is 9.64% with only 1 reference, 1.07% with 3.
//  Better to say too little than to turn into noise ——
//  a warning on every store gets ignored after the second time.
// ---------------------------------------------------------------------------
void ProfileBank::checkTimbreMismatch(const InstrumentProfile &np) {
  lastAddSuspect = false;
  lastAddDist    = 0.0f;
  if (n < TC_TIMBRE_WARN_MIN_REFS) return;

  float best = 1e9f;
  for (int i = 0; i < n; i++) {
    const float d = profileTimbreDistance(np, p[i]);
    if (d > 0.0f && d < best) best = d;
  }
  if (best > 1e8f) return;              // Nothing to compare against (too little overlapping band)

  lastAddDist = best;
  if (best < TC_TIMBRE_WARN_DIST) return;

  lastAddSuspect = true;
  Serial.println();
  Serial.printf("[BANK] 注意：這個音色跟庫裡現有的差異很大（%.1f，門檻 %.1f）。\n",
                best, TC_TIMBRE_WARN_DIST);
  Serial.println(F("       換樂器了嗎？換樂器要先清空音色庫（選單 Timbre -> Clear trainset，"));
  Serial.println(F("       或序列埠按 z），否則新舊音色會混在同一個庫裡 ——"));
  Serial.println(F("       合成時每個音各自挑最近的音高，聽起來就會一個音一種樂器。"));
  Serial.println(F("       如果只是同一把樂器的極端音域或不同奏法，忽略這則訊息即可。"));
}

bool ProfileBank::add(const InstrumentProfile &np) {
  if (!np.valid || np.f0 <= 0.0f) return false;

  checkTimbreMismatch(np);

  // Same pitch (within a semitone) just overwrites, so re-analysing one file won't flood the bank
  for (int i = 0; i < n; i++) {
    if (fabsf(1200.0f * log2f(np.f0 / p[i].f0)) < 50.0f) { p[i] = np; return true; }
  }
  if (n >= TC_MAX_PROFILES) {
    const int victim = evictionTarget(np.f0);
    if (victim < 0) {
      Serial.printf("[BANK] 已滿，而且 %.1f Hz 在已涵蓋的音區內，略過\n", np.f0);
      return false;
    }
    Serial.printf("[BANK] 已滿：%.1f Hz 換掉 %.1f Hz（讓音域分佈更平均）\n",
                  np.f0, p[victim].f0);
    p[victim] = np;
    return true;
  }
  p[n++] = np;
  return true;
}

// ---------------------------------------------------------------------------
//  Who gets sacrificed when the bank is full
//
//  Old version: simply refuse. Looks safe, isn't —— load order is "sorted by
//  filename", so out of 32 trumpet samples (E3~B5) what survived was
//  A3 A4 A5 Ab3 Ab4 Ab5 B3 B4 B5 … the 16 that happen to sort first
//  alphabetically, leaving E3~G3 with no material at all.
//  Measured harmonic LSD for those notes was 12~14 dB (>8 means "audibly a
//  different timbre"), while the mid-high register held three or four
//  near-duplicate notes.
//
//  Now: count the new entry in as well, find "the most crowded pair in pitch",
//  and sacrifice whichever of the two is the more redundant. The criterion is
//  transposition distance —— the value of a timbre bank is that "every note
//  finds a close enough sample", so minimise "the largest gap between
//  neighbours".
//
//  Endpoints get special protection: once the lowest or highest note of the
//  range is replaced, anything beyond it can only be reached by extrapolating a
//  transposition, which is much worse than interpolating. So only "non-endpoint"
//  entries are eligible to be sacrificed.
// ---------------------------------------------------------------------------
int ProfileBank::evictionTarget(float newF0) const {
  if (n < TC_MAX_PROFILES || newF0 <= 0.0f) return -1;

  // The goal is plain: make "the largest gap between neighbours" as small as possible.
  //
  // That number is how far we have to transpose in the worst case —— the whole value
  // of the timbre bank lies in "every note finds a close enough sample", so use it
  // directly as the objective function, not some indirect proxy such as "the most
  // crowded pair".
  //
  // The first version did use the indirect proxy (sacrifice whoever is most crowded
  // against its neighbour), and the desktop test caught it thrashing straight away:
  // the log read "349.2 Hz replaces 174.6 Hz", and 174.6 Hz was what it had taken in
  // one step earlier. Each step looks reasonable on its own; together they go in circles.
  //
  // n is at most 16, so O(n^2) is only 256 comparisons; just work out the result for
  // every candidate victim directly.

  // Existing pitches (semitones, relative to A4), sorted
  float cur[TC_MAX_PROFILES];
  for (int i = 0; i < n; i++) cur[i] = 12.0f * log2f(p[i].f0 / 440.0f);
  int idx[TC_MAX_PROFILES];
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 1; i < n; i++) {
    const int key = idx[i];
    int j = i - 1;
    while (j >= 0 && cur[idx[j]] > cur[key]) { idx[j + 1] = idx[j]; j--; }
    idx[j + 1] = key;
  }

  const float ns = 12.0f * log2f(newF0 / 440.0f);

  // Objective function: sum of squared gaps.
  //
  // Why not "largest gap": the largest gap has a broad plateau —— many candidate
  // swaps come out with exactly the same largest gap, so "only swap on a strict
  // improvement" gets stuck forever. Measured on 32 trumpet samples, using largest
  // gap as the objective stalled at 6 semitones, as bad as the old version.
  // The sum of squares has no plateau: move energy from a big gap to a small one and
  // it drops, so every step pushes towards an even distribution.
  auto cost = [](const float *sorted, int cnt, float *outRange) {
    float sum = 0.0f;
    for (int i = 1; i < cnt; i++) {
      const float d = sorted[i] - sorted[i - 1];
      sum += d * d;
    }
    if (outRange) *outRange = (cnt >= 2) ? (sorted[cnt - 1] - sorted[0]) : 0.0f;
    return sum;
  };

  float sortedCur[TC_MAX_PROFILES];
  for (int i = 0; i < n; i++) sortedCur[i] = cur[idx[i]];
  float curRange = 0.0f;
  const float curCost = cost(sortedCur, n, &curRange);

  // Is the new pitch outside the existing range? Notes beyond the range can only be
  // extrapolated, with no sample at all to work from, which is the worst case there is
  // —— so always take anything that widens the range, without looking at the sum of squares.
  const bool extendsRange = (ns < sortedCur[0] - 1e-4f) ||
                            (ns > sortedCur[n - 1] + 1e-4f);

  int   best = -1;
  float bestCost = 1e30f;

  // Endpoints are exempt: replace the lowest or highest note of the range and anything beyond it can only be extrapolated.
  for (int k = 1; k < n - 1; k++) {
    const int victim = idx[k];
    float cand[TC_MAX_PROFILES + 1];
    int   m = 0;
    for (int i = 0; i < n; i++) if (idx[i] != victim) cand[m++] = cur[idx[i]];
    int ins = m;
    for (int i = 0; i < m; i++) if (ns < cand[i]) { ins = i; break; }
    for (int i = m; i > ins; i--) cand[i] = cand[i - 1];
    cand[ins] = ns;
    m++;

    const float c = cost(cand, m, nullptr);
    if (c < bestCost) { bestCost = c; best = victim; }
  }
  if (best < 0) return -1;                       // n <= 2, no interior points

  if (extendsRange) return best;

  // Inside the range, only swap if the distribution genuinely gets more even.
  // Equal counts as no swap, deliberately: a swap that gains nothing only makes which
  // entry survives depend on load order, and load order is filename order —— that is
  // handing the result over to the filenames.
  return (bestCost < curCost - 1e-4f) ? best : -1;
}


int ProfileBank::nearest(float f0) const {
  if (n <= 0) return -1;
  int   best = 0;
  float bd   = 1e30f;
  for (int i = 0; i < n; i++) {
    float d = fabsf(log2f(f0 / p[i].f0));        // Distance in octaves, not a difference in Hz
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

const InstrumentProfile *ProfileBank::get(float f0) const {
  int i = nearest(f0);
  return (i < 0) ? nullptr : &p[i];
}

void ProfileBank::summary() const {
  if (n == 0) { Serial.println(F("[BANK] 音色庫是空的")); return; }
  static const char *nm[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
  Serial.printf("[BANK] %d 組音色：", n);
  for (int i = 0; i < n; i++) {
    int m = (int)lroundf(69.0f + 12.0f * log2f(p[i].f0 / 440.0f));
    m = (m < 0) ? 0 : (m > 127 ? 127 : m);
    Serial.printf("%s%d ", nm[((m % 12) + 12) % 12], m / 12 - 1);
  }
  Serial.println();
}

bool bankSave(const ProfileBank &b, const char *path) {
  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  int32_t n = b.n;
  f.write((const uint8_t *)&n, sizeof(n));
  for (int i = 0; i < b.n; i++)
    f.write((const uint8_t *)&b.p[i], sizeof(InstrumentProfile));
  f.close();
  Serial.printf("[BANK] 已存檔 %s（%d 組，%lu KB）\n", path, b.n,
                (unsigned long)((sizeof(int32_t) + (size_t)b.n * sizeof(InstrumentProfile)) / 1024));
  return true;
}

bool bankLoad(ProfileBank &b, const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  int32_t n = 0;
  if (f.read((uint8_t *)&n, sizeof(n)) != (int)sizeof(n) || n <= 0 || n > TC_MAX_PROFILES) {
    f.close();
    return false;
  }
  if (f.size() != sizeof(int32_t) + (uint32_t)n * sizeof(InstrumentProfile)) {
    Serial.println(F("[BANK] 檔案大小不符（格式已改？），忽略"));
    f.close();
    return false;
  }
  b.clear();
  for (int i = 0; i < n; i++) {
    InstrumentProfile tmp;
    f.read((uint8_t *)&tmp, sizeof(tmp));
    if (tmp.magic == TC_PROFILE_MAGIC) { tmp.valid = true; b.add(tmp); }
  }
  f.close();
  b.summary();
  return b.n > 0;
}

// ============================================================================
bool profileSave(const InstrumentProfile &p, const char *path) {
  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.write((const uint8_t *)&p, sizeof(InstrumentProfile));
  f.close();
  Serial.printf("[PROFILE] 已存檔 %s (%u bytes)\n", path, (unsigned)sizeof(InstrumentProfile));
  return true;
}

bool profileLoad(InstrumentProfile &p, const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  if (f.size() != sizeof(InstrumentProfile)) { f.close(); return false; }
  f.read((uint8_t *)&p, sizeof(InstrumentProfile));
  f.close();
  if (p.magic != TC_PROFILE_MAGIC) { p.valid = false; return false; }
  p.valid = true;
  Serial.printf("[PROFILE] 已載入 %s  f0=%.1f Hz\n", path, p.f0);
  return true;
}

void profilePrint(const InstrumentProfile &p) {
  if (!p.valid) { Serial.println(F("[PROFILE] (無效)")); return; }
  Serial.println(F("---------------- 音色指紋 ----------------"));
  Serial.printf("  f0          : %.2f Hz  (約 MIDI %.1f)\n",
                p.f0, 69.0f + 12.0f * log2f(p.f0 / 440.0f));
  Serial.printf("  音長        : %.2f s\n", p.noteDur);
  Serial.printf("  ADSR        : A=%.3f  D=%.3f  S=%.2f  R=%.3f\n",
                p.attack, p.decay, p.sustain, p.release);
  Serial.printf("  持續段衰減  : %.3f /秒  (%s)\n", p.sustainDecayPerSec,
                p.sustainDecayPerSec >= 0.9999f ? "長音型：管樂/弦樂/管風琴"
                                                : "衰減型：鋼琴/吉他/撥弦");
  Serial.printf("  噪聲比      : 持續 %.3f  起音 %.3f\n", p.noiseGain, p.attackNoise);
  Serial.printf("  非諧性 B    : %.5f\n", p.inharmonicity);
  Serial.printf("  亮度(質心)  : %.2f x f0\n", p.brightness);
  Serial.printf("  shimmer     : %.1f %%\n", p.shimmerDepth * 100.0f);
  if (p.vibratoCents < 2.0f) Serial.println(F("  顫音        : 無"));
  else Serial.printf("  顫音        : %.1f cents @ %.1f Hz\n", p.vibratoCents, p.vibratoHz);
  Serial.print (F("  起音延遲(ms): "));
  for (int h = 0; h < 12; h++) Serial.printf("%.0f ", p.harmOnset[h] * 1000.0f);
  Serial.println(F("..."));
  Serial.print (F("  諧波(起音)  : "));
  for (int h = 0; h < 12; h++) Serial.printf("%.3f ", p.keyframe[2][h]);
  Serial.println(F("..."));
  Serial.print (F("  諧波(持續)  : "));
  for (int h = 0; h < 12; h++) Serial.printf("%.3f ", p.keyframe[TC_N_KEYFRAME / 2][h]);
  Serial.println(F("..."));
  Serial.println(F("------------------------------------------"));
}

// ---------------------------------------------------------------------------
//  Spectral envelope distance
//
//  Three design decisions, all of them to avoid false positives (a false positive
//  is harder to live with than a miss —— a warning on every store gets ignored
//  after the second time):
//
//  1) Compare shape only. Each side subtracts its own mean over the comparison
//     interval before the difference is taken.
//     A different recording level or mic gain must not read as a change of instrument.
//
//  2) Compare only the band "above both fundamentals". How much this one matters
//     only showed up once it was measured:
//     starting from 200 Hz, B5 and B4 of the same trumpet came out 17.97 dB apart ——
//     B5 has an f0 of 1006 Hz, so 200 Hz~1 kHz holds no harmonics at all, only the
//     noise floor, and the shape of that noise is of course nothing like the real
//     envelope of a low note.
//     That is not a timbre difference, it is a register difference, yet it is enough
//     to have the warning screaming at one and the same instrument.
//
//  3) Cap at 8 kHz. Above that it is mostly noise, and plenty of samples were never
//     recorded that high in the first place.
// ---------------------------------------------------------------------------
float profileEnvDistance(const InstrumentProfile &a, const InstrumentProfile &b) {
  if (!a.valid || !b.valid) return 0.0f;

  const float lo = logf(TC_SPECENV_FMIN), hi = logf(TC_SPECENV_FMAX);
  const float step = (hi - lo) / (TC_SPECENV_PTS - 1);

  // Start a little above the higher of the two fundamentals. Below the fundamental there are no harmonics to measure.
  const float fLo = fmaxf(200.0f, 1.2f * fmaxf(a.f0, b.f0));
  const float fHi = 8000.0f;

  int   idx[TC_SPECENV_PTS], nUse = 0;
  float sumA = 0.0f, sumB = 0.0f;

  for (int p = 0; p < TC_SPECENV_PTS; p++) {
    const float fc = expf(lo + step * p);
    if (fc < fLo || fc > fHi) continue;
    if (a.specEnv[p] < -60.0f || b.specEnv[p] < -60.0f) continue;
    idx[nUse++] = p;
    sumA += a.specEnv[p];
    sumB += b.specEnv[p];
  }

  // Too little overlapping band and there is nothing to compare. Returning 0 means
  // "don't know" and the caller raises no warning —— when the evidence is thin, keep
  // quiet rather than guess.
  if (nUse < 8) return 0.0f;

  const float mA = sumA / nUse, mB = sumB / nUse;
  float acc = 0.0f;
  for (int i = 0; i < nUse; i++) {
    const float d = (a.specEnv[idx[i]] - mA) - (b.specEnv[idx[i]] - mB);
    acc += d * d;
  }
  return sqrtf(acc / nUse);
}

// ---------------------------------------------------------------------------
//  Combined timbre distance
//
//  Each term is divided by "the typical spread within one instrument" and then
//  squared and summed, so the output is roughly "how many times a normal
//  pitch-to-pitch difference this is". Around 1.0 = very likely the same instrument.
//
//  The scales come from measured data (33 trumpet notes, 12 each for piano/violin/
//  flute, 68 samples in all, 2278 pairings). The tool is tools/sim/envdist; rerun it
//  when the samples change.
//
//  To be honest about it: only 4 instruments, and the weights are hand-set, not
//  learned. It catches the obvious case, "winds swapped for a piano", but two
//  instruments of similar timbre (trumpet for trombone, say) it probably cannot tell
//  apart. This is a hint, not a verdict.
// ---------------------------------------------------------------------------
float profileTimbreDistance(const InstrumentProfile &a, const InstrumentProfile &b) {
  if (!a.valid || !b.valid) return 0.0f;

  auto term = [](float x, float y, float scale) {
    const float d = (x - y) / scale;
    return d * d;
  };

  float acc = 0.0f;
  int   n   = 0;

  // Spectral envelope: within-instrument nearest-neighbour median 2.26 dB, take 2.5 as the scale
  const float dEnv = profileEnvDistance(a, b);
  if (dEnv > 0.0f) { acc += term(dEnv, 0.0f, 2.5f); n++; }

  // Sustain decay: separates "the ones that decay" from "the ones that hold", piano 0.35 vs orchestral 0.96~1.00
  acc += term(a.sustainDecayPerSec, b.sustainDecayPerSec, 0.06f); n++;

  // Inharmonicity: piano 2.3e-4, winds 2e-5. Strings sit in between and vary a lot
  acc += term(a.inharmonicity, b.inharmonicity, 6.0e-5f); n++;

  // Harmonic-by-harmonic micro-ripple: violin/flute 0.10~0.13, trumpet 0.04, piano 0.00
  acc += term(a.shimmerDepth, b.shimmerDepth, 0.035f); n++;

  // Brightness uses a log ratio. A brass instrument's absolute centroid barely moves
  // with pitch, so brightness(=centroid/f0) gets smaller high up —— taking the log is
  // what keeps that pitch dependence from reading as a timbre difference.
  if (a.brightness > 0.05f && b.brightness > 0.05f) {
    acc += term(log2f(a.brightness), log2f(b.brightness), 0.55f); n++;
  }

  return sqrtf(acc / (float)n);
}

float specEnvGain(const InstrumentProfile &p, float hz) {
  if (hz <= TC_SPECENV_FMIN)  hz = TC_SPECENV_FMIN;
  if (hz >= TC_SPECENV_FMAX)  hz = TC_SPECENV_FMAX;

  const float lo = logf(TC_SPECENV_FMIN);
  const float hi = logf(TC_SPECENV_FMAX);
  float pos = (logf(hz) - lo) / (hi - lo) * (TC_SPECENV_PTS - 1);

  int   i = (int)pos;
  if (i > TC_SPECENV_PTS - 2) i = TC_SPECENV_PTS - 2;
  float t = pos - i;

  float db = p.specEnv[i] * (1.0f - t) + p.specEnv[i + 1] * t;
  return powf(10.0f, db / 20.0f);
}

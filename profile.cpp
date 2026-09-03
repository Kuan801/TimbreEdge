#include "profile.h"
#include <SD.h>

// ============================================================================
//  ProfileBank
// ============================================================================
// ---------------------------------------------------------------------------
//  入庫前的「這好像不是同一把樂器」檢查
//
//  比的是「跟庫裡最像的那一組」差多少，不是平均 —— 合成時本來就是挑音高
//  最接近的那一組來用，而且用平均會被鋼琴害慘（鋼琴同樂器內部差異比
//  「小號 vs 提琴」還大，因為每個音的琴弦與擊槌都不同）。
//
//  庫裡少於 TC_TIMBRE_WARN_MIN_REFS 組就不開口：實測只有 1 組當參考時
//  誤報率 9.64%，3 組降到 1.07%。寧可少講話也不要變成雜訊 ——
//  每次入庫都跳警告，看兩次就開始無視了。
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
  if (best > 1e8f) return;              // 沒得比（重疊頻段不足）

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

  // 同一個音高（半音以內）就直接覆蓋，重複分析同一個檔不會塞爆
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
//  庫滿了的時候該犧牲誰
//
//  舊版：直接拒收。看起來安全，其實不是 —— 載入順序是「檔名排序」，
//  所以 32 個小號素材（E3~B5）留下的是 A3 A4 A5 Ab3 Ab4 Ab5 B3 B4 B5 …
//  這種依字母排出來的 16 個，E3~G3 整段完全沒有素材。
//  實測那批音的諧波 LSD 是 12~14 dB（>8 就是「音色明顯不同」），
//  而中高音區同時塞了三四個幾乎重複的音。
//
//  現在改成：把新的那組也算進來，找出「音高上最擠的一對」，犧牲其中比較
//  冗餘的那個。判準是移調距離 —— 音色庫的價值就在於「每個音符都能找到
//  夠近的素材」，所以要最小化「最大的相鄰間距」。
//
//  端點特別保護：音域的最低與最高音一旦被換掉，超出去的音就只能靠移調外推，
//  那比內插差得多。所以只在「非端點」之間挑犧牲者。
// ---------------------------------------------------------------------------
int ProfileBank::evictionTarget(float newF0) const {
  if (n < TC_MAX_PROFILES || newF0 <= 0.0f) return -1;

  // 目標很明確：讓「最大的相鄰間距」越小越好。
  //
  // 那個數字就是最壞情況下要移調多遠 —— 音色庫的整個價值就在於
  // 「每個音符都能找到夠近的素材」，所以直接拿它當目標函數，
  // 不要用什麼「最擠的一對」之類的間接指標。
  //
  // 第一版就是用間接指標（犧牲跟鄰居最擠的那個），桌機測試立刻抓到它會抖：
  // log 裡出現「349.2 Hz 換掉 174.6 Hz」，而 174.6 Hz 是它前一步才剛收進來的。
  // 每一步各自看起來合理，合起來卻在原地繞。
  //
  // n 最多 16，O(n^2) 也才 256 次比較，直接把每個候選犧牲者的結果算出來就好。

  // 現有音高（半音，相對 A4），排序
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

  // 目標函數：間距平方和。
  //
  // 為什麼不是「最大間距」：最大間距有一大片平台區 —— 很多次替換算出來的
  // 最大間距完全相同，於是「只有嚴格變好才換」就永遠卡在原地。實測 32 個
  // 小號素材用最大間距當目標，最後停在 6 個半音，跟舊版一樣爛。
  // 平方和沒有平台：只要把能量從大間距搬到小間距，它就會下降，
  // 所以每一步都推向平均分佈。
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

  // 新的音高在現有範圍之外嗎？超出範圍的音只能靠外推，沒有任何素材可以參考，
  // 是所有情況裡最差的一種 —— 所以擴大音域一律收，不看平方和。
  const bool extendsRange = (ns < sortedCur[0] - 1e-4f) ||
                            (ns > sortedCur[n - 1] + 1e-4f);

  int   best = -1;
  float bestCost = 1e30f;

  // 端點不參選：音域的最低與最高一旦被換掉，超出去的音就只能往外推。
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
  if (best < 0) return -1;                       // n <= 2，沒有內部點

  if (extendsRange) return best;

  // 範圍內的話，要真的讓分佈更平均才換。
  // 相等就不換是刻意的：沒有好處的替換只會讓最後留下哪一組取決於載入順序，
  // 而載入順序是檔名排序 —— 那等於把結果交給檔名決定。
  return (bestCost < curCost - 1e-4f) ? best : -1;
}


int ProfileBank::nearest(float f0) const {
  if (n <= 0) return -1;
  int   best = 0;
  float bd   = 1e30f;
  for (int i = 0; i < n; i++) {
    float d = fabsf(log2f(f0 / p[i].f0));        // 用八度距離，不是 Hz 差
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
//  頻譜包絡距離
//
//  三個設計決定，每個都是為了避免誤報（誤報比漏報難忍受 —— 每次入庫都跳
//  警告，看兩次就開始無視了）：
//
//  1) 只比形狀。兩邊各自扣掉自己在比較區間內的平均值再算差。
//     錄音音量或麥克風增益不同不該被當成換了樂器。
//
//  2) 只比「兩邊基頻以上」的頻段。這一條是量出來才知道有多重要：
//     一開始從 200 Hz 起算，結果同一把小號的 B5 跟 B4 距離高達 17.97 dB ——
//     因為 B5 的 f0 是 1006 Hz，200 Hz~1 kHz 那段根本沒有諧波，只有噪聲底，
//     而那段噪聲的形狀跟低音的真實包絡當然完全不同。
//     那不是音色差異，是音域差異，卻足以讓警告在同一把樂器上狂叫。
//
//  3) 上限 8 kHz。再上去主要是噪聲，而且很多素材本來就沒錄到那麼高。
// ---------------------------------------------------------------------------
float profileEnvDistance(const InstrumentProfile &a, const InstrumentProfile &b) {
  if (!a.valid || !b.valid) return 0.0f;

  const float lo = logf(TC_SPECENV_FMIN), hi = logf(TC_SPECENV_FMAX);
  const float step = (hi - lo) / (TC_SPECENV_PTS - 1);

  // 起點取兩者較高的基頻再往上一點。低於基頻的地方沒有諧波可量。
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

  // 重疊的頻段太少就沒得比。回 0 代表「不知道」，呼叫端不會發警告 ——
  // 資訊不足時保持安靜，比亂猜好。
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
//  綜合音色距離
//
//  每一項都除以「同樂器內部的典型差異」再平方相加，所以輸出大約是
//  「相當於幾倍的正常音高差異」。1.0 附近 = 很可能是同一把樂器。
//
//  尺度是從實測資料訂的（小號 33 音、鋼琴/提琴/長笛各 12 音，共 68 個素材，
//  2278 組配對）。工具是 tools/sim/envdist，素材換了要重跑。
//
//  誠實說明：只有 4 種樂器、權重是手訂的，不是學出來的。
//  它抓得到「管樂換成鋼琴」這種明顯的情況，但兩種音色相近的樂器
//  （例如小號換長號）大概分不出來。這是提示，不是判定。
// ---------------------------------------------------------------------------
float profileTimbreDistance(const InstrumentProfile &a, const InstrumentProfile &b) {
  if (!a.valid || !b.valid) return 0.0f;

  auto term = [](float x, float y, float scale) {
    const float d = (x - y) / scale;
    return d * d;
  };

  float acc = 0.0f;
  int   n   = 0;

  // 頻譜包絡：同樂器最近鄰中位數 2.26 dB，取 2.5 當尺度
  const float dEnv = profileEnvDistance(a, b);
  if (dEnv > 0.0f) { acc += term(dEnv, 0.0f, 2.5f); n++; }

  // 持續段衰減：分辨「會衰減的」與「能持續的」，鋼琴 0.35 vs 管弦 0.96~1.00
  acc += term(a.sustainDecayPerSec, b.sustainDecayPerSec, 0.06f); n++;

  // 非諧性：鋼琴 2.3e-4，管樂 2e-5。弦樂居中且變異大
  acc += term(a.inharmonicity, b.inharmonicity, 6.0e-5f); n++;

  // 逐諧波微觀起伏：提琴/長笛 0.10~0.13，小號 0.04，鋼琴 0.00
  acc += term(a.shimmerDepth, b.shimmerDepth, 0.035f); n++;

  // 亮度用 log 比值。銅管的絕對質心幾乎不隨音高變，所以 brightness(=質心/f0)
  // 在高音會變小 —— 取 log 之後這個音高相依性才不會被當成音色差異。
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

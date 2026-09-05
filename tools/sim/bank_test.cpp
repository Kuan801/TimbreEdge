// ============================================================================
//  bank_test.cpp  -  the timbre bank's admission rules and the "instrument changed" warning
//
//  Usage:  make bank_test && ./bank_test
//
//  The thresholds themselves were measured on real material (see the comments in
//  config.h and tools/sim/envdist); what is verified here is that the mechanism is
//  right:
//    - same pitch overwrites, different pitch accumulates
//    - refuse once full
//    - stay quiet while there are not enough reference entries
//    - speak up only on a big difference, stay silent when it is close
//
//  Synthetic profiles rather than real WAVs, so this test does not depend on
//  material files -- they live on the user's desktop, and on another machine it
//  would not run at all.
// ============================================================================
#include "Arduino.h"
#include "../../profile.h"
#include <cstdio>
#include <cmath>

static int gFail = 0;
static void check(const char *what, bool ok) {
  printf("  %-52s %s\n", what, ok ? "通過" : "**失敗**");
  if (!ok) gFail++;
}

// Build a profile. rolloff sets the slope of the spectral envelope; decay/inharm/shimmer
// are the ones that really separate instruments (measured: piano 0.35/2.3e-4/0.00, winds 0.96/2e-5/0.04~0.10)
static InstrumentProfile mk(float f0, float rolloff, float decay,
                            float inharm, float shimmer, float bright) {
  InstrumentProfile p{};
  p.valid = true;
  p.f0    = f0;
  p.sustainDecayPerSec = decay;
  p.inharmonicity      = inharm;
  p.shimmerDepth       = shimmer;
  p.brightness         = bright;

  const float lo = logf(TC_SPECENV_FMIN), hi = logf(TC_SPECENV_FMAX);
  for (int i = 0; i < TC_SPECENV_PTS; i++) {
    const float fc = expf(lo + (hi - lo) * i / (TC_SPECENV_PTS - 1));
    float db = (fc < f0) ? -70.0f : -rolloff * log2f(fc / f0);
    if (db < -72.0f) db = -72.0f;
    if (db >  0.0f)  db =   0.0f;
    p.specEnv[i] = db;
  }
  return p;
}

// "different pitches of the same wind instrument"
static InstrumentProfile wind(float f0)  { return mk(f0, 6.0f, 0.96f, 2.0e-5f, 0.05f, 4.0f); }
// "piano": fast decay, high inharmonicity, almost no shimmer
static InstrumentProfile piano(float f0) { return mk(f0, 9.0f, 0.35f, 2.3e-4f, 0.00f, 2.2f); }

int main() {
  printf("\n=== 音色庫入庫規則測試 ===\n\n");

  printf("1) 同音高覆蓋、不同音高累加\n");
  {
    ProfileBank b;
    b.add(wind(261.6f));                       // C4
    check("加入第一組後 n=1", b.n == 1);
    b.add(wind(293.7f));                       // D4
    check("不同音高會累加，n=2", b.n == 2);
    InstrumentProfile again = wind(262.5f);    // less than a semitone from C4
    again.brightness = 9.9f;                   // A marker, to confirm it really was replaced
    b.add(again);
    check("半音以內視為同一個音，n 不變", b.n == 2);
    bool replaced = false;
    for (int i = 0; i < b.n; i++) if (b.p[i].brightness == 9.9f) replaced = true;
    check("而且內容真的被換成新的", replaced);
  }

  printf("\n2) 滿了之後：只換掉冗餘的，不會無腦踢最舊的\n");
  {
    // This section used to assert "return false once full". That contract has
    // changed -- refusing looks safe, but it really hands the outcome to filename
    // sorting: of 32 trumpet samples the 16 kept were the alphabetically first ones,
    // all of E3~G3 had no material at all, and those notes measured 12~14 dB
    // harmonic LSD. The eviction logic itself is tested thoroughly in evict_test;
    // what is guarded here is only the invariants: capacity must not be blown, the
    // endpoints must not disappear.
    // Spacing has to be even in *semitones*, not even in Hz.
    //
    // The first version used 200 + i*40 Hz and the test caught it immediately: that
    // is not remotely even in log pitch (200->240 is 3.16 semitones, 760->800 only
    // 0.89), so slipping in another pitch down in the low register really did make
    // the distribution more even -- the code was right to judge it "worth swapping",
    // it was my premise that was wrong.
    ProfileBank b;
    for (int i = 0; i < TC_MAX_PROFILES; i++)
      b.add(wind(200.0f * powf(2.0f, i / 12.0f)));
    check("填滿到 TC_MAX_PROFILES（半音均勻）", b.n == TC_MAX_PROFILES);

    const float loF0 = b.p[0].f0;
    const float hiF0 = b.p[TC_MAX_PROFILES - 1].f0;
    // Already even in semitones: another pitch inside the range would only leave one stretch at 2 semitones -> refused
    const bool ok = b.add(wind(200.0f * powf(2.0f, 0.5f / 12.0f)));
    check("範圍內、無助於分佈的音高仍然拒收", !ok);
    check("n 永遠不超過 TC_MAX_PROFILES", b.n == TC_MAX_PROFILES);

    // A pitch that widens the range must be taken, but not at the cost of an endpoint
    b.add(wind(3000.0f));
    check("擴大音域之後 n 仍然是上限", b.n == TC_MAX_PROFILES);
    bool loKept = false, hiKept = false, newIn = false;
    for (int i = 0; i < b.n; i++) {
      if (b.p[i].f0 == loF0)   loKept = true;
      if (b.p[i].f0 == hiF0)   hiKept = true;
      if (b.p[i].f0 == 3000.0f) newIn = true;
    }
    check("原本的最低音沒有被犧牲", loKept);
    check("新的最高音有進來", newIn);
    (void)hiKept;
  }

  printf("\n3) 換樂器警告：參考組數不足時不開口\n");
  {
    // Measured: with only 1 reference entry the false alarm rate is 9.64%, and it
    // takes 3 to get down to 1.07%. Better to say less -- a warning on every
    // admission gets ignored after the second sighting.
    ProfileBank b;
    for (int i = 0; i < TC_TIMBRE_WARN_MIN_REFS - 1; i++)
      b.add(wind(261.6f + i * 40.0f));
    b.add(piano(392.0f));                      // clearly a different instrument
    check("庫裡不足 3 組時，再怎麼不像也不出聲", !b.lastAddSuspect);
  }

  printf("\n4) 換樂器警告：夠多參考時，差很多要出聲\n");
  {
    ProfileBank b;
    b.add(wind(261.6f)); b.add(wind(329.6f)); b.add(wind(392.0f)); b.add(wind(440.0f));
    check("同樂器的第 5 個音不出聲", (b.add(wind(493.9f)), !b.lastAddSuspect));
    b.add(piano(349.2f));
    check("換成鋼琴會出聲", b.lastAddSuspect);
    check("距離有記錄下來給面板用", b.lastAddDist >= TC_TIMBRE_WARN_DIST);
  }

  printf("\n5) 反過來也要成立（鋼琴庫加進管樂）\n");
  {
    ProfileBank b;
    b.add(piano(261.6f)); b.add(piano(329.6f)); b.add(piano(392.0f)); b.add(piano(440.0f));
    check("同為鋼琴的第 5 個音不出聲", (b.add(piano(493.9f)), !b.lastAddSuspect));
    b.add(wind(349.2f));
    check("換成管樂會出聲", b.lastAddSuspect);
  }

  printf("\n6) clear() 要把警告狀態一起清掉\n");
  {
    ProfileBank b;
    for (int i = 0; i < 4; i++) b.add(wind(261.6f + i * 40.0f));
    b.add(piano(349.2f));
    check("先製造一次警告", b.lastAddSuspect);
    b.clear();
    check("clear 之後 n 歸零", b.n == 0);
    check("clear 之後警告狀態也歸零", !b.lastAddSuspect && b.lastAddDist == 0.0f);
    // After a clear and a fresh start, the first few notes must not still be carrying the previous run's warning
    b.add(piano(261.6f));
    check("清空後加入第一個音不出聲", !b.lastAddSuspect);
  }

  printf("\n7) 音色距離的基本性質\n");
  {
    const InstrumentProfile a = wind(261.6f), c = wind(392.0f), pn = piano(261.6f);
    check("自己跟自己距離為 0", profileTimbreDistance(a, a) < 1e-4f);
    check("對稱", fabsf(profileTimbreDistance(a, pn) - profileTimbreDistance(pn, a)) < 1e-4f);
    check("同樂器不同音高 < 換樂器",
          profileTimbreDistance(a, c) < profileTimbreDistance(a, pn));
    InstrumentProfile bad{}; bad.valid = false;
    check("無效的 profile 回 0（不知道就別亂猜）",
          profileTimbreDistance(a, bad) == 0.0f);
  }

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

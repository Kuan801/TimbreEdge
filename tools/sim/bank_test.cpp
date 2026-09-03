// ============================================================================
//  bank_test.cpp  -  音色庫的入庫規則與「換樂器」警告
//
//  用法：  make bank_test && ./bank_test
//
//  門檻值本身是用真實素材量出來的（見 config.h 的註解與 tools/sim/envdist），
//  這裡驗的是「機制對不對」：
//    - 同音高覆蓋、不同音高累加
//    - 滿了之後拒收
//    - 參考組數不足時不開口
//    - 差很多才開口，像的時候要安靜
//
//  用合成的 profile 而不是真實 WAV，是為了讓這個測試不依賴素材檔案 ——
//  素材放在使用者的桌面上，換一台電腦就跑不動了。
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

// 造一個 profile。rolloff 決定頻譜包絡的斜率，decay/inharm/shimmer 是
// 真正把樂器分開的那幾項（實測：鋼琴 0.35/2.3e-4/0.00，管樂 0.96/2e-5/0.04~0.10）
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

// 「同一把管樂器的不同音高」
static InstrumentProfile wind(float f0)  { return mk(f0, 6.0f, 0.96f, 2.0e-5f, 0.05f, 4.0f); }
// 「鋼琴」：衰減快、非諧性高、幾乎沒有 shimmer
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
    InstrumentProfile again = wind(262.5f);    // 跟 C4 差不到半音
    again.brightness = 9.9f;                   // 做個記號，確認真的換掉了
    b.add(again);
    check("半音以內視為同一個音，n 不變", b.n == 2);
    bool replaced = false;
    for (int i = 0; i < b.n; i++) if (b.p[i].brightness == 9.9f) replaced = true;
    check("而且內容真的被換成新的", replaced);
  }

  printf("\n2) 滿了之後：只換掉冗餘的，不會無腦踢最舊的\n");
  {
    // 這一節原本斷言「滿了就回 false」。那個契約已經改了 ——
    // 拒收看起來安全，實際上是把結果交給檔名排序決定：32 個小號素材
    // 留下的是依字母排的 16 個，E3~G3 整段沒有素材，那批音的諧波 LSD
    // 量到 12~14 dB。取捨邏輯本身在 evict_test 有完整測試，
    // 這裡只守住「不變量」：容量不能被撐破、端點不能消失。
    // 間距要在「半音」上均勻，不是在 Hz 上均勻。
    //
    // 第一版寫成 200 + i*40 Hz，測試立刻抓到問題：那在對數音高上一點都不均勻
    // （200->240 是 3.16 個半音，760->800 只有 0.89 個），所以再塞一個低音區的
    // 音高進來確實會讓分佈更平均，程式判斷「值得換」是對的，錯的是我的前提。
    ProfileBank b;
    for (int i = 0; i < TC_MAX_PROFILES; i++)
      b.add(wind(200.0f * powf(2.0f, i / 12.0f)));
    check("填滿到 TC_MAX_PROFILES（半音均勻）", b.n == TC_MAX_PROFILES);

    const float loF0 = b.p[0].f0;
    const float hiF0 = b.p[TC_MAX_PROFILES - 1].f0;
    // 已經半音均勻了，再塞一個範圍內的音高只會讓某一段變成 2 個半音 -> 拒收
    const bool ok = b.add(wind(200.0f * powf(2.0f, 0.5f / 12.0f)));
    check("範圍內、無助於分佈的音高仍然拒收", !ok);
    check("n 永遠不超過 TC_MAX_PROFILES", b.n == TC_MAX_PROFILES);

    // 擴大音域的音高要收，但端點不能被犧牲
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
    // 實測只有 1 組當參考時誤報率 9.64%，3 組才降到 1.07%。
    // 寧可少講話 —— 每次入庫都跳警告，看兩次就開始無視了。
    ProfileBank b;
    for (int i = 0; i < TC_TIMBRE_WARN_MIN_REFS - 1; i++)
      b.add(wind(261.6f + i * 40.0f));
    b.add(piano(392.0f));                      // 明顯是別的樂器
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
    // 清空後重新開始，前幾個音不該還掛著上一輪的警告
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

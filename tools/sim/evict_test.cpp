// ============================================================================
//  evict_test  -  音色庫滿了之後該犧牲誰
//
//  這段邏輯是無聲失效的那一類：換錯人不會當機、不會報錯，只會讓某個音區
//  沒有素材可用，然後所有落在那一區的音都靠遠距離移調硬撐。
//  實測小號那批就是這樣 —— E3~G3 整段被丟光，諧波 LSD 12~14 dB。
//
//  所以測試分三種：
//    1) 真實情境（32 個小號素材）最後留下來的分佈夠不夠平均
//    2) 每一條規則的正例與負例
//    3) 負對照：舊版邏輯（滿了就拒收）在同一份輸入下必須明顯更差
// ============================================================================
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "../../config.h"
#include "../../profile.h"

static int gFail = 0;
static void check(const char *name, bool ok, const char *note = "") {
  printf("  %-50s %s %s\n", name, ok ? "通過" : "失敗 <<<<", note);
  if (!ok) gFail++;
}

static ProfileBank gB;

static InstrumentProfile mk(float f0) {
  InstrumentProfile p;
  memset(&p, 0, sizeof(p));
  p.magic = TC_PROFILE_MAGIC;
  p.valid = true;
  p.f0 = f0;
  p.noteDur = 2.0f;
  // 讓「換樂器」的偵測不要在這裡插嘴：所有 profile 的頻譜包絡都一樣，
  // 距離為 0，checkTimbreMismatch 不會有意見。這個測試只關心音高取捨。
  for (int i = 0; i < TC_SPECENV_PTS; i++) p.specEnv[i] = -20.0f;
  for (int k = 0; k < TC_N_KEYFRAME; k++) {
    p.loud[k] = 1.0f;
    for (int h = 0; h < TC_N_HARM; h++) p.keyframe[k][h] = (h == 0) ? 1.0f : 0.0f;
  }
  return p;
}

static float midiToHz(int m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }

// 庫裡「最大的相鄰間距」—— 這就是最壞情況下要移調多遠
static float maxGapSemis(const ProfileBank &b) {
  std::vector<float> v;
  for (int i = 0; i < b.n; i++) v.push_back(12.0f * log2f(b.p[i].f0 / 440.0f));
  std::sort(v.begin(), v.end());
  float g = 0;
  for (size_t i = 1; i < v.size(); i++) g = std::max(g, v[i] - v[i - 1]);
  return g;
}
static float rangeSemis(const ProfileBank &b) {
  float lo = 1e9f, hi = -1e9f;
  for (int i = 0; i < b.n; i++) {
    const float s = 12.0f * log2f(b.p[i].f0 / 440.0f);
    lo = std::min(lo, s); hi = std::max(hi, s);
  }
  return hi - lo;
}

// 檔名排序（A, Ab, B, Bb, C, D, Db, E, Eb, F, G, Gb）產生的載入順序 ——
// 跟音高完全無關，這正是舊版留下爛分佈的原因
static std::vector<int> trumpetLoadOrder() {
  const int semis[] = {9, 8, 11, 10, 0, 2, 1, 4, 3, 5, 7, 6};
  std::vector<int> midis;
  for (int ni = 0; ni < 12; ni++)
    for (int oct = 3; oct <= 5; oct++) {
      const int m = 12 * (oct + 1) + semis[ni];
      if (m >= 52 && m <= 83) midis.push_back(m);      // E3 ~ B5
    }
  return midis;
}

int main() {
  printf("\n音色庫滿了之後的取捨\n");

  // -------------------------------------------------------------------------
  printf("\n1) 真實情境：32 個小號素材（E3~B5），依檔名排序載入\n");
  {
    gB.clear();
    for (int m : trumpetLoadOrder()) gB.add(mk(midiToHz(m)));

    char msg[96];
    snprintf(msg, sizeof(msg), "(收了 %d 組，音域 %.0f 個半音，最大間距 %.0f 個半音)",
             gB.n, rangeSemis(gB), maxGapSemis(gB));
    check("庫被填滿", gB.n == TC_MAX_PROFILES, msg);
    check("音域仍然涵蓋整個 E3~B5（端點沒被犧牲）", rangeSemis(gB) >= 30.0f, msg);
    // 31 個半音塞 16 組，理想的最大間距約 2~3 個半音
    check("最大間距 <= 4 個半音", maxGapSemis(gB) <= 4.0f, msg);
  }

  // -------------------------------------------------------------------------
  printf("\n2) 負對照：舊版「滿了就拒收」在同一份輸入下\n");
  {
    ProfileBank old;
    old.clear();
    for (int m : trumpetLoadOrder())
      if (old.n < TC_MAX_PROFILES) old.p[old.n++] = mk(midiToHz(m));

    char msg[96];
    snprintf(msg, sizeof(msg), "(舊版最大間距 %.0f 個半音，新版 %.0f)",
             maxGapSemis(old), maxGapSemis(gB));
    check("舊版的最大間距確實比較差", maxGapSemis(old) > maxGapSemis(gB), msg);
  }

  // -------------------------------------------------------------------------
  printf("\n3) 規則的正例與負例\n");
  {
    // 均勻鋪滿 C4 起算的 16 個半音
    gB.clear();
    for (int i = 0; i < TC_MAX_PROFILES; i++) gB.add(mk(midiToHz(60 + i)));
    check("先填滿 16 組", gB.n == 16);

    // (a) 落在範圍內、而且那一段本來就很擠 -> 不值得換
    const int v1 = gB.evictionTarget(midiToHz(60) * powf(2.0f, 0.5f / 12.0f));
    check("範圍內、間距只有 1 個半音 -> 不換", v1 < 0);

    // (b) 擴大音域 -> 一定要收
    const int v2 = gB.evictionTarget(midiToHz(84));
    check("高過整個音域 -> 換掉某一組", v2 >= 0);
    const int v3 = gB.evictionTarget(midiToHz(40));
    check("低於整個音域 -> 換掉某一組", v3 >= 0);

    // (c) 端點不能被犧牲
    int loIdx = 0, hiIdx = 0;
    for (int i = 0; i < gB.n; i++) {
      if (gB.p[i].f0 < gB.p[loIdx].f0) loIdx = i;
      if (gB.p[i].f0 > gB.p[hiIdx].f0) hiIdx = i;
    }
    check("犧牲者不是最低音", v2 != loIdx && v3 != loIdx);
    check("犧牲者不是最高音", v2 != hiIdx && v3 != hiIdx);
  }
  {
    // (d) 範圍內但那一段是個大洞 -> 值得換
    gB.clear();
    gB.add(mk(midiToHz(48)));                                  // C3，撐住下端
    for (int i = 0; i < 14; i++) gB.add(mk(midiToHz(72 + i))); // C5~ 密集
    gB.add(mk(midiToHz(96)));                                  // C7，撐住上端
    char msg[64]; snprintf(msg, sizeof(msg), "(共 %d 組)", gB.n);
    check("填滿 16 組", gB.n == 16, msg);
    const int v = gB.evictionTarget(midiToHz(60));  // C4，落在 48~72 的大洞正中間
    check("補一個 24 半音大洞的中點 -> 值得換", v >= 0);
    if (v >= 0)
      check("犧牲的是密集區裡的那一組",
            gB.p[v].f0 > midiToHz(71) && gB.p[v].f0 < midiToHz(86));
  }

  // -------------------------------------------------------------------------
  printf("\n4) 不會退步：沒滿的時候行為完全不變\n");
  {
    gB.clear();
    for (int i = 0; i < 12; i++) gB.add(mk(midiToHz(60 + i)));   // 鋼琴那批的規模
    check("12 組全部收下", gB.n == 12);
    check("沒滿的時候不會有人被換掉", gB.evictionTarget(midiToHz(50)) < 0);
    // 同音高覆蓋的既有行為要保持
    gB.add(mk(midiToHz(60)));
    check("同音高仍然是覆蓋而不是新增", gB.n == 12);
  }

  // -------------------------------------------------------------------------
  printf("\n5) 壞輸入\n");
  {
    check("f0 <= 0 不會被當成有效目標", gB.evictionTarget(0.0f) < 0);
    gB.clear();
    check("空庫不會爆", gB.evictionTarget(440.0f) < 0);
    gB.add(mk(440.0f));
    check("只有一組時不會犧牲它", gB.evictionTarget(880.0f) < 0);
  }

  printf(gFail ? "\n有 %d 項失敗\n" : "\n全部通過\n", gFail);
  return gFail ? 1 : 0;
}

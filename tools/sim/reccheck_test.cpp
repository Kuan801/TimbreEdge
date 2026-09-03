// ============================================================================
//  reccheck_test  -  單音錄音判定的規則測試
//
//  這個測試存在的理由：判定門檻是「靜靜地失效」的那一類程式碼。改錯了不會
//  當機、不會編不過，只會從此不再攔下錄壞的檔，而那要等到合成出來不像才會
//  被發現 —— 中間可能已經錄了幾十個音。
//
//  所以每一條規則都要有正例也要有負例：光證明「壞的會被抓到」不夠，
//  還要證明「好的不會被誤判」，否則把門檻全部設成 0 也能通過測試。
// ============================================================================
#include "../../rec_check.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int gFail = 0;

static void check(const char *name, bool ok, const char *note = "") {
  printf("  %-44s %s %s\n", name, ok ? "通過" : "失敗 <<<<", note);
  if (!ok) gFail++;
}

// 一份「錄得很好」的基準：乾淨素材（Iowa MIS 鋼琴 C4）量到的數量級
static RecCheck good() {
  RecCheck r;
  r.analysisOk  = true;
  r.peak        = 0.45f;
  r.clipRatio   = 0.0f;
  r.noiseFloor  = 0.004f;      // 訊噪比約 48 dB
  r.onsets      = 1;
  r.noteDur     = 1.80f;
  r.f0          = 261.6f;
  r.decayPerSec = 0.55f;
  return r;
}

static RecVerdict eval(const RecCheck &r, char *reason = nullptr, size_t cap = 0) {
  char fix[26];
  return recCheckEval(r, reason, cap, fix, sizeof(fix));
}

int main() {
  printf("\n單音錄音判定\n");

  printf("\n1) 基準：錄得好的要判合格\n");
  {
    char reason[26];
    const RecVerdict v = eval(good(), reason, sizeof(reason));
    check("乾淨素材 -> REC_OK", v == REC_OK, reason);
    check("合格時仍給得出摘要", reason[0] != 0, reason);
  }

  printf("\n2) 不能用的四種情況（都來自實際錄壞的檔案）\n");
  {
    RecCheck r = good(); r.analysisOk = false;
    check("分析失敗 -> BAD", eval(r) == REC_BAD);
  }
  {
    RecCheck r = good(); r.onsets = 6;          // 實測吉他連撥 6 下
    char reason[26];
    check("一次錄到 6 個音 -> BAD", eval(r, reason, sizeof(reason)) == REC_BAD, reason);
  }
  {
    // 第一份 REC.WAV：峰值 1.000、203/88200 個削波樣本 = 0.23%
    RecCheck r = good(); r.peak = 1.0f; r.clipRatio = 0.0023f;
    char reason[26];
    check("削波 0.23% -> BAD", eval(r, reason, sizeof(reason)) == REC_BAD, reason);
  }
  {
    // 第二份 REC.WAV：峰值 0.089、訊噪比 7 dB
    RecCheck r = good(); r.peak = 0.089f; r.noiseFloor = 0.45f;
    char reason[26];
    check("訊噪比 7 dB -> BAD", eval(r, reason, sizeof(reason)) == REC_BAD, reason);
  }
  {
    RecCheck r = good(); r.noteDur = 0.20f;
    check("音長 0.2 秒 -> BAD", eval(r) == REC_BAD);
  }

  printf("\n3) 邊界：門檻兩側要真的分得開\n");
  {
    RecCheck a = good(); a.clipRatio = 0.0004f;   // 門檻 0.0005 之下
    RecCheck b = good(); b.clipRatio = 0.0006f;
    check("削波 0.04% 不判 BAD", eval(a) != REC_BAD);
    check("削波 0.06% 判 BAD",   eval(b) == REC_BAD);
  }
  {
    // 「太小聲」要峰值與訊噪比同時不合格才算數（見第 8 節的誤判紀錄），
    // 所以這裡的兩份素材都把底噪拉到 25 dB，才是在測峰值那一半的門檻
    RecCheck a = good(); a.peak = 0.06f; a.noiseFloor = 0.056f;   // 25 dB
    RecCheck b = good(); b.peak = 0.04f; b.noiseFloor = 0.056f;
    check("峰值 0.06 不判 BAD", eval(a) != REC_BAD);
    check("峰值 0.04 判 BAD",   eval(b) == REC_BAD);
  }
  {
    RecCheck a = good(); a.noiseFloor = 0.09f;    // 訊噪比 21 dB
    RecCheck b = good(); b.noiseFloor = 0.11f;    // 訊噪比 19 dB
    check("訊噪比 21 dB 不判 BAD", eval(a) != REC_BAD);
    check("訊噪比 19 dB 判 BAD",   eval(b) == REC_BAD);
  }

  printf("\n4) 提醒（能用但可以更好）\n");
  {
    RecCheck r = good(); r.peak = 0.95f;
    check("峰值 0.95 -> WARN（還沒削波）", eval(r) == REC_WARN);
  }
  {
    RecCheck r = good(); r.peak = 0.10f;
    check("峰值 0.10 -> WARN", eval(r) == REC_WARN);
  }
  {
    RecCheck r = good(); r.noteDur = 0.6f;
    check("音長 0.6 秒 -> WARN", eval(r) == REC_WARN);
  }
  {
    // 管風琴 / 長弓弦樂是真的不衰減，所以只提醒不否決
    RecCheck r = good(); r.decayPerSec = 0.99f;
    check("量不到衰減 -> WARN 而不是 BAD", eval(r) == REC_WARN);
  }

  printf("\n5) 優先順序：同時中好幾條時，先講最好改的那一條\n");
  {
    // 削波又太短：削波是使用者按一下 g 就能改的，音長要重錄
    RecCheck r = good(); r.clipRatio = 0.01f; r.noteDur = 0.1f;
    char reason[26];
    eval(r, reason, sizeof(reason));
    check("削波排在音長之前", strstr(reason, "clip") != nullptr, reason);
  }
  {
    // 多個音又削波：多個音會讓整份 profile 失效，比削波更根本
    RecCheck r = good(); r.onsets = 3; r.clipRatio = 0.01f;
    char reason[26];
    eval(r, reason, sizeof(reason));
    check("起音次數排在削波之前", strstr(reason, "notes") != nullptr, reason);
  }

  printf("\n6) 輸出字串的長度限制（OLED 一行 21 個字元）\n");
  {
    const RecCheck cases[] = {
      good(),
      { true, 1.0f, 0.9999f, 0.004f, 1, 1.8f, 261.6f, 0.55f },
      { true, 0.001f, 0.0f, 0.99f, 99, 0.01f, 27.5f, 0.999f },
      { false, 0.0f, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f },
      // 每一條規則各來一發，確保沒有任何一句訊息超出面板寬度
      { true, 0.001f, 0.0f,    0.004f, 1, 1.8f, 261.6f, 0.55f },   // 太小聲
      { true, 0.45f,  0.0f,    0.45f,  1, 1.8f, 261.6f, 0.55f },   // 噪音大
      { true, 0.45f,  0.0f,    0.004f, 1, 0.1f, 261.6f, 0.55f },   // 太短
      { true, 0.95f,  0.0f,    0.004f, 1, 1.8f, 261.6f, 0.55f },   // 太大聲
      { true, 0.10f,  0.0f,    0.004f, 1, 1.8f, 261.6f, 0.55f },   // 偏小聲
      { true, 0.45f,  0.0f,    0.05f,  1, 1.8f, 261.6f, 0.55f },   // 訊噪比普通
      { true, 0.45f,  0.0f,    0.004f, 1, 0.6f, 261.6f, 0.55f },   // 稍短
      { true, 0.45f,  0.0f,    0.004f, 1, 1.8f, 261.6f, 0.99f },   // 量不到衰減
    };
    bool fits = true;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      char reason[26], fix[26];
      recCheckEval(cases[i], reason, sizeof(reason), fix, sizeof(fix));
      if (strlen(reason) > 21 || strlen(fix) > 21) {
        fits = false;
        printf("     過長：\"%s\" / \"%s\"\n", reason, fix);
      }
    }
    check("極端數值下兩行都塞得進 21 字元", fits);
  }

  printf("\n7) 訊噪比換算與「量不到」\n");
  {
    check("0.1 -> 20 dB",  fabsf(recCheckSnrDb(0.1f)  - 20.0f) < 0.01f);
    check("0.01 -> 40 dB", fabsf(recCheckSnrDb(0.01f) - 40.0f) < 0.01f);
    check("0 代表量不到", !recCheckSnrKnown(0.0f));
    check("0.004 是量得到的", recCheckSnrKnown(0.004f));
    // 量不到的時候不能回 inf 或 NaN 去餵給 snprintf
    const float q = recCheckSnrDb(0.0f);
    check("量不到時回有限的哨兵值", q == RECCHK_SNR_UNKNOWN);
  }

  printf("\n8) 真實素材抓到的兩個誤判（回歸測試）\n");
  {
    // Iowa MIS 的鋼琴原始素材：峰值只有 0.033，但訊噪比 40 dB。
    // 這是拿來當基準的乾淨錄音，判成「不能用」是錯的。
    RecCheck r = good(); r.peak = 0.033f; r.noiseFloor = 0.01f; r.noteDur = 3.25f;
    char reason[26];
    const RecVerdict v = eval(r, reason, sizeof(reason));
    check("乾淨但電平低的母帶不判 BAD", v != REC_BAD, reason);
    check("但仍會提醒電平偏低",        v == REC_WARN, reason);
  }
  {
    // 峰值低「而且」訊噪比也差，才是真的不能用
    RecCheck r = good(); r.peak = 0.033f; r.noiseFloor = 0.09f;   // 21 dB
    check("電平低又吵 -> BAD", eval(r) == REC_BAD);
  }
  {
    // 連續採樣模式的前置緩衝會讓起音落在第 0 格，量不到底噪。
    // 那時候不能因為「看起來 SNR 很好」就給合格，也不能因為量不到就判失敗。
    RecCheck r = good(); r.noiseFloor = 0.0f;
    check("量不到 SNR 時仍依其他指標判定", eval(r) == REC_OK);
    RecCheck q = good(); q.noiseFloor = 0.0f; q.clipRatio = 0.01f;
    check("量不到 SNR 不影響其他規則",     eval(q) == REC_BAD);
  }

  printf(gFail ? "\n有 %d 項失敗\n" : "\n全部通過\n", gFail);
  return gFail ? 1 : 0;
}

#include "../../rec_check.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int gFail = 0;

static void check(const char *name, bool ok, const char *note = "") {
  printf("  %-44s %s %s\n", name, ok ? "通過" : "失敗 <<<<", note);
  if (!ok) gFail++;
}

static RecCheck good() {
  RecCheck r;
  r.analysisOk  = true;
  r.peak        = 0.45f;
  r.clipRatio   = 0.0f;
  r.noiseFloor  = 0.004f;
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
    RecCheck r = good(); r.onsets = 6;
    char reason[26];
    check("一次錄到 6 個音 -> BAD", eval(r, reason, sizeof(reason)) == REC_BAD, reason);
  }
  {

    RecCheck r = good(); r.peak = 1.0f; r.clipRatio = 0.0023f;
    char reason[26];
    check("削波 0.23% -> BAD", eval(r, reason, sizeof(reason)) == REC_BAD, reason);
  }
  {

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
    RecCheck a = good(); a.clipRatio = 0.0004f;
    RecCheck b = good(); b.clipRatio = 0.0006f;
    check("削波 0.04% 不判 BAD", eval(a) != REC_BAD);
    check("削波 0.06% 判 BAD",   eval(b) == REC_BAD);
  }
  {

    RecCheck a = good(); a.peak = 0.06f; a.noiseFloor = 0.056f;
    RecCheck b = good(); b.peak = 0.04f; b.noiseFloor = 0.056f;
    check("峰值 0.06 不判 BAD", eval(a) != REC_BAD);
    check("峰值 0.04 判 BAD",   eval(b) == REC_BAD);
  }
  {
    RecCheck a = good(); a.noiseFloor = 0.09f;
    RecCheck b = good(); b.noiseFloor = 0.11f;
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

    RecCheck r = good(); r.decayPerSec = 0.99f;
    check("量不到衰減 -> WARN 而不是 BAD", eval(r) == REC_WARN);
  }

  printf("\n5) 優先順序：同時中好幾條時，先講最好改的那一條\n");
  {

    RecCheck r = good(); r.clipRatio = 0.01f; r.noteDur = 0.1f;
    char reason[26];
    eval(r, reason, sizeof(reason));
    check("削波排在音長之前", strstr(reason, "clip") != nullptr, reason);
  }
  {

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

      { true, 0.001f, 0.0f,    0.004f, 1, 1.8f, 261.6f, 0.55f },
      { true, 0.45f,  0.0f,    0.45f,  1, 1.8f, 261.6f, 0.55f },
      { true, 0.45f,  0.0f,    0.004f, 1, 0.1f, 261.6f, 0.55f },
      { true, 0.95f,  0.0f,    0.004f, 1, 1.8f, 261.6f, 0.55f },
      { true, 0.10f,  0.0f,    0.004f, 1, 1.8f, 261.6f, 0.55f },
      { true, 0.45f,  0.0f,    0.05f,  1, 1.8f, 261.6f, 0.55f },
      { true, 0.45f,  0.0f,    0.004f, 1, 0.6f, 261.6f, 0.55f },
      { true, 0.45f,  0.0f,    0.004f, 1, 1.8f, 261.6f, 0.99f },
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

    const float q = recCheckSnrDb(0.0f);
    check("量不到時回有限的哨兵值", q == RECCHK_SNR_UNKNOWN);
  }

  printf("\n8) 真實素材抓到的兩個誤判（回歸測試）\n");
  {

    RecCheck r = good(); r.peak = 0.033f; r.noiseFloor = 0.01f; r.noteDur = 3.25f;
    char reason[26];
    const RecVerdict v = eval(r, reason, sizeof(reason));
    check("乾淨但電平低的母帶不判 BAD", v != REC_BAD, reason);
    check("但仍會提醒電平偏低",        v == REC_WARN, reason);
  }
  {

    RecCheck r = good(); r.peak = 0.033f; r.noiseFloor = 0.09f;
    check("電平低又吵 -> BAD", eval(r) == REC_BAD);
  }
  {

    RecCheck r = good(); r.noiseFloor = 0.0f;
    check("量不到 SNR 時仍依其他指標判定", eval(r) == REC_OK);
    RecCheck q = good(); q.noiseFloor = 0.0f; q.clipRatio = 0.01f;
    check("量不到 SNR 不影響其他規則",     eval(q) == REC_BAD);
  }

  printf(gFail ? "\n有 %d 項失敗\n" : "\n全部通過\n", gFail);
  return gFail ? 1 : 0;
}

// ============================================================================
//  trigger_test  -  連續採樣觸發判斷的狀態機測試
//
//  這個測試是為了一個實際回報的症狀寫的：「沒有播放聲音的狀況下會連續不斷
//  地收音」。那個 bug 在真機上要坐在旁邊等好幾分鐘才看得到，而且看到了也
//  不知道是門檻的問題還是重新武裝的問題 —— 在桌機上餵合成訊號，兩秒跑完
//  全部路徑，而且分得出來是哪一條。
//
//  背景噪音的數字全部來自使用者實際錄到的 12 個檔（每個取最安靜的 0.3 秒，
//  共 1236 個 block）：中位 0.0207、99% 0.0281、最大 0.0301。
//  最弱的那個音 block 峰值中位 0.0529。
// ============================================================================
#include "../../trigger.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int gFail = 0;
static void check(const char *name, bool ok, const char *note = "") {
  printf("  %-46s %s %s\n", name, ok ? "通過" : "失敗 <<<<", note);
  if (!ok) gFail++;
}

// 一個 block = 128 取樣 @44.1 kHz = 2.902 ms。時間用整數毫秒累加會失真，
// 所以用浮點累加再取整 —— 不然 400 ms 的門檻會差到十幾個 block。
struct Clock {
  double ms = 0;
  uint32_t tick() { ms += 128.0 / 44.1; return (uint32_t)ms; }
};

// 可重現的偽亂數，不要用 rand()（各平台實作不同，測試就不可重現了）
static uint32_t gSeed = 12345;
static float urand() {
  gSeed = gSeed * 1103515245u + 12345u;
  return (float)((gSeed >> 16) & 0x7FFF) / 32767.0f;
}

// 背景噪音的 block 峰值：中位 0.021、最大 0.030。
// 用「中位 + 隨機起伏」逼近實測的波峰因數 1.46。
static float noiseBlock(float scale = 1.0f) {
  return scale * (0.0175f + 0.0130f * urand());
}

// 餵 N 毫秒的訊號，回傳這段期間觸發了幾次
static int feedFor(TriggerGate &g, Clock &c, double ms, float (*gen)(float), float scale) {
  int n = 0;
  const double until = c.ms + ms;
  while (c.ms < until) {
    const uint32_t t = c.tick();
    if (g.feed(gen(scale), t)) n++;
  }
  return n;
}

static float genNoise(float s) { return noiseBlock(s); }
static float genSilent(float s) { (void)s; return 0.0008f; }   // 幾乎沒訊號
static float genNote(float s)  { return s; }                   // 穩定的音

int main() {
  printf("\n連續採樣的觸發判斷\n");

  // -------------------------------------------------------------------------
  printf("\n1) 回報的症狀：沒有人演奏，只有背景噪音\n");
  {
    // 把背景放大到 1.4 倍（0.025~0.042），也就是「峰值會超過原本固定門檻
    // 0.035」的那種房間 —— 這正是會整晚亂錄的情況。
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    const int n = feedFor(g, c, 30000.0, genNoise, 1.4f);
    char msg[80];
    snprintf(msg, sizeof(msg), "(30 秒，環境 %.4f，門檻自動抬到 %.4f)",
             g.ambient(), g.threshold());
    check("純背景噪音 30 秒，一次都不觸發", n == 0, msg);
    check("門檻確實被環境抬高了", g.threshold() > 0.035f);
  }
  {
    // 背景剛好卡在「高於半個門檻、低於門檻」的死角 —— 舊版就是在這裡
    // 永遠武裝、一有風吹草動就錄。
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 5000.0, genNoise, 1.0f);
    check("背景介於半門檻與門檻之間時不會亂觸發",
          feedFor(g, c, 20000.0, genNoise, 1.0f) == 0);
  }

  // -------------------------------------------------------------------------
  printf("\n2) 正對照：真的有音的時候必須錄得到\n");
  {
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genNoise, 1.0f);          // 校正 + 武裝
    const int n = feedFor(g, c, 300.0, genNote, 0.0529f);   // 實測最弱的那個音
    char msg[80];
    snprintf(msg, sizeof(msg), "(環境 %.4f，門檻 %.4f，音 0.0529)",
             g.ambient(), g.threshold());
    check("最弱的音（0.0529）觸發得了", n >= 1, msg);
  }
  {
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genSilent, 1.0f);
    check("安靜的房間裡，普通音量的音也觸發得了",
          feedFor(g, c, 300.0, genNote, 0.20f) >= 1);
  }

  // -------------------------------------------------------------------------
  printf("\n3) 一個音只能觸發一次（尾巴不能再觸發）\n");
  {
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genSilent, 1.0f);
    int n = feedFor(g, c, 100.0, genNote, 0.30f);        // 起音
    g.noteRecorded(c.tick());                            // 錄完 2 秒
    n += feedFor(g, c, 1200.0, genNote, 0.08f);          // 還在響的尾巴
    check("錄完之後尾巴不會再觸發一次", n == 1);
  }
  {
    // 但是安靜下來之後，下一個音要能觸發 —— 不然採樣模式只能收一個音
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genSilent, 1.0f);
    feedFor(g, c, 100.0, genNote, 0.30f);
    g.noteRecorded(c.tick());
    feedFor(g, c, 800.0, genSilent, 1.0f);               // 停半秒以上
    check("安靜之後下一個音收得到",
          feedFor(g, c, 200.0, genNote, 0.30f) == 1);
  }
  {
    // 錄完之後「馬上」又出現一個音（沒有停頓）不該收 ——
    // 那多半是同一個音，或是使用者彈太快，兩者都不該當成新素材
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genSilent, 1.0f);
    feedFor(g, c, 100.0, genNote, 0.30f);
    g.noteRecorded(c.tick());
    feedFor(g, c, 100.0, genSilent, 1.0f);               // 只停 100 ms
    check("只停 100 ms 不足以重新武裝（要 400 ms）",
          feedFor(g, c, 200.0, genNote, 0.30f) == 0);
  }

  // -------------------------------------------------------------------------
  printf("\n4) 校正期\n");
  {
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    check("剛 arm 完是校正中", g.calibrating());
    const int n = feedFor(g, c, TC_TRIG_CAL_MS - 50.0, genNote, 0.5f);
    check("校正期間就算很大聲也不觸發", n == 0);
    check("校正期間仍然是校正中", g.calibrating());
    feedFor(g, c, 200.0, genSilent, 1.0f);
    check("校正期過了就結束", !g.calibrating());
  }
  {
    // 校正期間一直很吵 -> 不該直接武裝，那不是「觀察到的安靜」
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, TC_TRIG_CAL_MS + 30.0, genNote, 0.5f);
    check("校正期間很吵的話不會立刻武裝", !g.armedReady());
  }
  {
    // 校正期間很安靜 -> 不用再多等 400 ms，直接可以收
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, TC_TRIG_CAL_MS + 30.0, genSilent, 1.0f);
    check("校正期間很安靜的話直接武裝（不用再等）", g.armedReady());
  }

  // -------------------------------------------------------------------------
  printf("\n5) 門檻下限：安靜的房間不該把門檻壓到 0\n");
  {
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 5000.0, genSilent, 1.0f);
    char msg[48]; snprintf(msg, sizeof(msg), "(門檻 %.4f)", g.threshold());
    check("極安靜時門檻仍不低於設定值", g.threshold() >= 0.035f - 1e-6f, msg);
    check("極小的訊號不會觸發", feedFor(g, c, 500.0, genNote, 0.01f) == 0);
  }

  // -------------------------------------------------------------------------
  printf("\n6) 訊噪餘裕的回報\n");
  {
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genNoise, 1.0f);
    const int n = feedFor(g, c, 200.0, genNote, 0.21f);   // 約為環境的 10 倍
    const float hr = g.lastHeadroomDb();
    char msg[48]; snprintf(msg, sizeof(msg), "(觸發 %d 次，%.1f dB)", n, hr);
    // 一定要先確認真的觸發了 —— 沒觸發的話 lastHeadroomDb() 回 0，
    // 「小於 12 dB」那條測試就會用錯誤的理由通過
    check("餘裕約 20 dB", n >= 1 && hr > 16.0f && hr < 24.0f, msg);
  }
  {
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genNoise, 1.0f);
    const int n = feedFor(g, c, 200.0, genNote, 0.053f);   // 使用者實際的情況
    const float hr = g.lastHeadroomDb();
    char msg[72]; snprintf(msg, sizeof(msg), "(觸發 %d 次，%.1f dB，提醒門檻 %.1f dB)",
                           n, hr, TC_TRIG_MIN_HEADROOM_DB);
    check("使用者目前的素材餘裕不足，會被提醒",
          n >= 1 && hr > 0.0f && hr < TC_TRIG_MIN_HEADROOM_DB, msg);
  }
  {
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genSilent, 1.0f);
    feedFor(g, c, 200.0, genNote, 0.30f);
    check("環境量到接近 0 時回有限值，不是 inf",
          g.lastHeadroomDb() > 0.0f && g.lastHeadroomDb() < 1000.0f);
  }

  // -------------------------------------------------------------------------
  printf("\n7) 負對照：把安全機制拆掉的話，測試必須要失敗\n");
  {
    // 直接模擬舊版的邏輯，確認上面第 1 節那個測試真的有鑑別力 ——
    // 不然「通過」可能只是因為測試根本沒踩到那條路。
    uint32_t quietSince = 0, now = 0; int hot = 0, trig = 0;
    Clock c; gSeed = 999;
    const float thresh = 0.035f;
    while (c.ms < 30000.0) {
      now = c.tick();
      const float pk = noiseBlock(1.4f);
      if (pk < thresh * 0.5f) quietSince = now;
      if (pk >= thresh) {
        if (++hot >= 2 && (now - quietSince) > TC_REARM_SILENT_MS) { trig++; hot = 0; }
      } else hot = 0;
    }
    char msg[64]; snprintf(msg, sizeof(msg), "(舊版在同樣的噪音下觸發了 %d 次)", trig);
    check("舊版邏輯在同一段噪音下確實會亂觸發", trig > 0, msg);
  }

  // ---------------------------------------------------------------------
  printf("\n8) 校正期撞到孤立尖峰（實機回報的症狀）\n");
  {
    // 實機紀錄：安靜時交流 RMS 只有 0.001，但輸入偶爾會有孤立的數位尖峰
    // 到 0.28。校正的 600 ms 撞上一個，環境就被量成 0.2767、門檻 0.415，
    // 之後怎麼彈都不會觸發，而畫面只會說「等待中… 電平 0.06 / 門檻 0.41」。
    TriggerGate g; Clock c; gSeed = 4242;
    g.arm(TC_TRIG_LEVEL, (uint32_t)c.ms);
    int n = 0;
    while (g.calibrating()) {
      // 每 80 個 block 插一個尖峰，600 ms 內大約 2~3 個
      const float pk = (++n % 80 == 0) ? 0.28f : noiseBlock(1.0f);
      g.feed(pk, c.tick());
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "(環境 %.4f，門檻 %.4f；尖峰是 0.28)",
             g.ambient(), g.threshold());
    check("尖峰不會把環境估到訊號等級", g.ambient() < 0.08f, msg);
    check("門檻仍然在可以觸發的範圍", g.threshold() < 0.12f, msg);

    // 正對照：這樣的門檻底下，正常力度的音還是要觸發得到
    feedFor(g, c, 600, genSilent, 1.0f);            // 先安靜一段把武裝湊滿
    const int trig = feedFor(g, c, 300, genNote, 0.20f);
    snprintf(msg, sizeof(msg), "(觸發 %d 次)", trig);
    check("修好之後 0.20 的音觸發得到", trig >= 1, msg);
  }

  printf("\n9) 負對照：環境改用最大值的話，同一段訊號會壞掉\n");
  {
    // 沒有這一段的話，第 8 節「通過」有可能只是因為那些尖峰根本沒被餵進去。
    // 這裡直接重現舊的估計方式：校正期間取最大值。
    Clock c; gSeed = 4242;
    float ambMax = 0.0f;
    int n = 0;
    double until = 0.0 + TC_TRIG_CAL_MS;
    while (c.ms < until) {
      const float pk = (++n % 80 == 0) ? 0.28f : noiseBlock(1.0f);
      if (pk > ambMax) ambMax = pk;
      c.tick();
    }
    const float oldThresh = ambMax * TC_TRIG_MARGIN;
    char msg[96];
    snprintf(msg, sizeof(msg), "(舊估計法：環境 %.4f -> 門檻 %.4f，0.20 的音進不去)",
             ambMax, oldThresh);
    check("取最大值確實會被一個尖峰毀掉", oldThresh > 0.20f, msg);
  }

  // ---------------------------------------------------------------------
  printf("\n10) block 電平：單根脈衝不該被當成有人在彈\n");
  {
    // 實機數據：安靜時 block RMS 只有 0.002，卻有 4~8%% 的 block 帶著一根
    // 0.19~0.28 的針（數位耦合）。用最大值看，這種 block 跟真的樂音沒兩樣。
    // TC_BLOCK 會展開成 AUDIO_BLOCK_SAMPLES（Audio.h 的東西），這支測試
    // 刻意不依賴 Arduino 的標頭，所以這裡自己寫死 128。
    const int BLK = 128;
    int16_t blk[BLK];

    // (a) 純樂音（440 Hz，振幅 0.20）
    for (int i = 0; i < BLK; i++)
      blk[i] = (int16_t)(0.20f * 32767.0f * sinf(2.0f * (float)M_PI * 440.0f * i / 44100.0f));
    const float lvlTone = tcBlockLevel(blk, BLK);
    char msg[96];
    snprintf(msg, sizeof(msg), "(量到 %.4f，實際振幅 0.20)", lvlTone);
    check("純樂音的電平幾乎等於振幅", lvlTone > 0.19f && lvlTone <= 0.201f, msg);

    // (b) 安靜 + 一根 0.28 的針
    for (int i = 0; i < BLK; i++) blk[i] = (int16_t)(0.002f * 32767.0f * urand());
    blk[37] = (int16_t)(0.28f * 32767.0f);
    const float lvlSpike = tcBlockLevel(blk, BLK);
    snprintf(msg, sizeof(msg), "(針是 0.28，量到 %.4f)", lvlSpike);
    check("單根脈衝被剔除，電平回到底噪", lvlSpike < 0.01f, msg);

    // (c) 三根針也還是要擋掉（TC_BLOCK_TOPK = 4）
    blk[60] = (int16_t)(0.25f * 32767.0f);
    blk[95] = (int16_t)(0.31f * 32767.0f);
    const float lvl3 = tcBlockLevel(blk, BLK);
    snprintf(msg, sizeof(msg), "(三根針，量到 %.4f)", lvl3);
    check("三根針也擋得住", lvl3 < 0.01f, msg);

    // (d) 樂音 + 針：電平仍由樂音決定，不會被針抬高
    for (int i = 0; i < BLK; i++)
      blk[i] = (int16_t)(0.20f * 32767.0f * sinf(2.0f * (float)M_PI * 440.0f * i / 44100.0f));
    blk[11] = (int16_t)(0.90f * 32767.0f);
    const float lvlBoth = tcBlockLevel(blk, BLK);
    snprintf(msg, sizeof(msg), "(樂音 0.20 + 0.90 的針 -> %.4f)", lvlBoth);
    check("有針也不會把樂音的電平灌高", lvlBoth < 0.25f, msg);

    // 負對照：取最大值的話，(b) 那個 block 看起來就像 0.28 的訊號
    float mx = 0.0f;
    for (int i = 0; i < BLK; i++) blk[i] = (int16_t)(0.002f * 32767.0f * urand());
    blk[37] = (int16_t)(0.28f * 32767.0f);
    for (int i = 0; i < BLK; i++) {
      const float a = fabsf(blk[i] / 32768.0f);
      if (a > mx) mx = a;
    }
    snprintf(msg, sizeof(msg), "(取最大值會量到 %.4f)", mx);
    check("負對照：取最大值確實會把針當成訊號", mx > 0.25f, msg);
  }

  printf(gFail ? "\n有 %d 項失敗\n" : "\n全部通過\n", gFail);
  return gFail ? 1 : 0;
}

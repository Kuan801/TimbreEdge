// ============================================================================
//  trigger_test  -  state machine test for the continuous-sampling trigger
//
//  This test was written for an actually reported symptom: "it keeps recording
//  continuously even when nothing is being played". On the real hardware that
//  bug takes several minutes of sitting next to the box to see, and even once
//  you see it you cannot tell whether it is the threshold or the re-arming —
//  on the desktop you feed it synthetic signal, every path runs in two seconds,
//  and it tells you which one.
//
//  All the background-noise numbers come from 12 files the user actually
//  recorded (the quietest 0.3 s of each, 1236 blocks in total): median 0.0207,
//  99% 0.0281, max 0.0301. The weakest note has a median block peak of 0.0529.
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

// One block = 128 samples @44.1 kHz = 2.902 ms. Accumulating the time in integer
// milliseconds drifts, so accumulate in float and round — otherwise the 400 ms
// threshold is off by a dozen blocks or more.
struct Clock {
  double ms = 0;
  uint32_t tick() { ms += 128.0 / 44.1; return (uint32_t)ms; }
};

// Reproducible PRNG — don't use rand() (the implementation differs per platform,
// which would make the test irreproducible)
static uint32_t gSeed = 12345;
static float urand() {
  gSeed = gSeed * 1103515245u + 12345u;
  return (float)((gSeed >> 16) & 0x7FFF) / 32767.0f;
}

// Background noise block peaks: median 0.021, max 0.030.
// "Median + random fluctuation" approximates the measured crest factor of 1.46.
static float noiseBlock(float scale = 1.0f) {
  return scale * (0.0175f + 0.0130f * urand());
}

// Feed N milliseconds of signal, return how many times it triggered during it
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
static float genSilent(float s) { (void)s; return 0.0008f; }   // almost no signal
static float genNote(float s)  { return s; }                   // a steady note

int main() {
  printf("\n連續採樣的觸發判斷\n");

  // -------------------------------------------------------------------------
  printf("\n1) 回報的症狀：沒有人演奏，只有背景噪音\n");
  {
    // Scale the background up 1.4x (0.025~0.042), i.e. the kind of room whose
    // peaks exceed the old fixed 0.035 threshold — exactly the case that records
    // junk all night long.
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
    // Background sitting right in the dead zone, above half the threshold but
    // below the threshold — this is where the old version stayed armed forever
    // and recorded at the slightest draught.
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
    feedFor(g, c, 1500.0, genNoise, 1.0f);          // calibrate + arm
    const int n = feedFor(g, c, 300.0, genNote, 0.0529f);   // the weakest note actually measured
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
    int n = feedFor(g, c, 100.0, genNote, 0.30f);        // attack
    g.noteRecorded(c.tick());                            // 2 s of recording done
    n += feedFor(g, c, 1200.0, genNote, 0.08f);          // tail still sounding
    check("錄完之後尾巴不會再觸發一次", n == 1);
  }
  {
    // But once it goes quiet the next note has to trigger — otherwise sampling
    // mode only ever captures one note
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genSilent, 1.0f);
    feedFor(g, c, 100.0, genNote, 0.30f);
    g.noteRecorded(c.tick());
    feedFor(g, c, 800.0, genSilent, 1.0f);               // silence for over half a second
    check("安靜之後下一個音收得到",
          feedFor(g, c, 200.0, genNote, 0.30f) == 1);
  }
  {
    // A note appearing immediately after a recording (with no gap) must not be
    // captured — it is most likely the same note, or the user playing too fast,
    // and neither should count as new material
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genSilent, 1.0f);
    feedFor(g, c, 100.0, genNote, 0.30f);
    g.noteRecorded(c.tick());
    feedFor(g, c, 100.0, genSilent, 1.0f);               // only 100 ms of silence
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
    // Noisy throughout the calibration -> must not arm straight away, that is not
    // "observed silence"
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, TC_TRIG_CAL_MS + 30.0, genNote, 0.5f);
    check("校正期間很吵的話不會立刻武裝", !g.armedReady());
  }
  {
    // Quiet throughout the calibration -> no need to wait another 400 ms, ready
    // to record right away
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
    const int n = feedFor(g, c, 200.0, genNote, 0.21f);   // roughly 10x the ambient level
    const float hr = g.lastHeadroomDb();
    char msg[48]; snprintf(msg, sizeof(msg), "(觸發 %d 次，%.1f dB)", n, hr);
    // Confirm it really did trigger first — with no trigger lastHeadroomDb()
    // returns 0 and the "less than 12 dB" test would pass for the wrong reason
    check("餘裕約 20 dB", n >= 1 && hr > 16.0f && hr < 24.0f, msg);
  }
  {
    TriggerGate g; Clock c;
    g.arm(0.035f, c.tick());
    feedFor(g, c, 1500.0, genNoise, 1.0f);
    const int n = feedFor(g, c, 200.0, genNote, 0.053f);   // the user's actual situation
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
    // Simulate the old logic directly, to confirm the test in section 1 above
    // really does discriminate — otherwise "pass" might only mean the test never
    // went down that path.
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
    // From the hardware log: when quiet the AC RMS is only 0.001, but the input
    // occasionally shows isolated digital spikes up to 0.28. If the 600 ms
    // calibration hits one, ambient is measured as 0.2767 and the threshold as
    // 0.415; after that nothing you play will trigger, and all the screen says is
    // "waiting… level 0.06 / threshold 0.41".
    TriggerGate g; Clock c; gSeed = 4242;
    g.arm(TC_TRIG_LEVEL, (uint32_t)c.ms);
    int n = 0;
    while (g.calibrating()) {
      // one spike every 80 blocks, roughly 2~3 within 600 ms
      const float pk = (++n % 80 == 0) ? 0.28f : noiseBlock(1.0f);
      g.feed(pk, c.tick());
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "(環境 %.4f，門檻 %.4f；尖峰是 0.28)",
             g.ambient(), g.threshold());
    check("尖峰不會把環境估到訊號等級", g.ambient() < 0.08f, msg);
    check("門檻仍然在可以觸發的範圍", g.threshold() < 0.12f, msg);

    // Positive control: under a threshold like that, a normally played note must
    // still trigger
    feedFor(g, c, 600, genSilent, 1.0f);            // a quiet stretch first, to complete the arming
    const int trig = feedFor(g, c, 300, genNote, 0.20f);
    snprintf(msg, sizeof(msg), "(觸發 %d 次)", trig);
    check("修好之後 0.20 的音觸發得到", trig >= 1, msg);
  }

  printf("\n9) 負對照：環境改用最大值的話，同一段訊號會壞掉\n");
  {
    // Without this, section 8 "passing" could just be because those spikes never
    // got fed in at all. This reproduces the old estimator directly: take the
    // maximum over the calibration window.
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
    // Hardware data: when quiet the block RMS is only 0.002, yet 4~8%% of blocks
    // carry a single 0.19~0.28 needle (digital coupling). Judged by the maximum,
    // such a block is indistinguishable from a real note.
    // TC_BLOCK expands to AUDIO_BLOCK_SAMPLES (an Audio.h thing), and this test
    // deliberately does not depend on the Arduino headers, so 128 is hard-coded
    // here.
    const int BLK = 128;
    int16_t blk[BLK];

    // (a) pure tone (440 Hz, amplitude 0.20)
    for (int i = 0; i < BLK; i++)
      blk[i] = (int16_t)(0.20f * 32767.0f * sinf(2.0f * (float)M_PI * 440.0f * i / 44100.0f));
    const float lvlTone = tcBlockLevel(blk, BLK);
    char msg[96];
    snprintf(msg, sizeof(msg), "(量到 %.4f，實際振幅 0.20)", lvlTone);
    check("純樂音的電平幾乎等於振幅", lvlTone > 0.19f && lvlTone <= 0.201f, msg);

    // (b) quiet + a single 0.28 needle
    for (int i = 0; i < BLK; i++) blk[i] = (int16_t)(0.002f * 32767.0f * urand());
    blk[37] = (int16_t)(0.28f * 32767.0f);
    const float lvlSpike = tcBlockLevel(blk, BLK);
    snprintf(msg, sizeof(msg), "(針是 0.28，量到 %.4f)", lvlSpike);
    check("單根脈衝被剔除，電平回到底噪", lvlSpike < 0.01f, msg);

    // (c) three needles must still be rejected (TC_BLOCK_TOPK = 4)
    blk[60] = (int16_t)(0.25f * 32767.0f);
    blk[95] = (int16_t)(0.31f * 32767.0f);
    const float lvl3 = tcBlockLevel(blk, BLK);
    snprintf(msg, sizeof(msg), "(三根針，量到 %.4f)", lvl3);
    check("三根針也擋得住", lvl3 < 0.01f, msg);

    // (d) tone + needles: the level is still set by the tone, not lifted by them
    for (int i = 0; i < BLK; i++)
      blk[i] = (int16_t)(0.20f * 32767.0f * sinf(2.0f * (float)M_PI * 440.0f * i / 44100.0f));
    blk[11] = (int16_t)(0.90f * 32767.0f);
    const float lvlBoth = tcBlockLevel(blk, BLK);
    snprintf(msg, sizeof(msg), "(樂音 0.20 + 0.90 的針 -> %.4f)", lvlBoth);
    check("有針也不會把樂音的電平灌高", lvlBoth < 0.25f, msg);

    // Negative control: taking the maximum, block (b) looks like a 0.28 signal
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

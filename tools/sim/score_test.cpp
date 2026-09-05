// ============================================================================
//  score_test.cpp  -  verify the two scores (chromatic scale and canon) on the desktop
//
//  Usage:  make score_test && ./score_test
//
//  A score is a pure data generator: no Arduino needed, no audio either. Writing
//  100-odd notes by hand easily produces "more overlap than there are voices",
//  "a tick left empty" or "a pitch too far outside the timbre bank", and flashing
//  it and listening makes those harder to locate, not easier.
// ============================================================================
#include "../../score.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

static int gFail = 0;
static void check(const char *what, bool ok, const char *detail = "") {
  printf("  %-46s %s %s\n", what, ok ? "通過" : "**失敗**", detail);
  if (!ok) gFail++;
}

static ScoreNote gN[TC_MAX_NOTES];

// How many notes are sounding at a given tick
static int polyAt(const ScoreNote *n, int cnt, int tick) {
  int p = 0;
  for (int i = 0; i < cnt; i++)
    if (tick >= n[i].tick && tick < n[i].tick + n[i].dur) p++;
  return p;
}

static void report() {
  const int cnt = buildScore(gN, TC_MAX_NOTES);
  const uint16_t total = scoreTotalTicks();
  char msg[80];

  printf("\n=== %s ===\n", scoreName());
  snprintf(msg, sizeof(msg), "(%d 個音符，%u tick)", cnt, total);
  check("有產生音符", cnt > 0, msg);
  check("沒有塞爆 TC_MAX_NOTES", cnt < TC_MAX_NOTES);

  // Pitch range
  int lo = 127, hi = 0;
  for (int i = 0; i < cnt; i++) { lo = std::min(lo, (int)gN[i].midi); hi = std::max(hi, (int)gN[i].midi); }
  snprintf(msg, sizeof(msg), "(%d ~ %d)", lo, hi);
  check("音高都在 MIDI 合法範圍", lo >= 0 && hi <= 127, msg);

  // The timbre bank only has C4~B4 (60~71); transpose too far and quality drops
  const int down = 60 - lo, up = hi - 71;
  snprintf(msg, sizeof(msg), "(往下 %d、往上 %d 個半音)", down > 0 ? down : 0, up > 0 ? up : 0);
  check("移調距離不超過 12 個半音", (down <= 12) && (up <= 12), msg);

  // Every note has to fall within the total length
  bool inRange = true;
  for (int i = 0; i < cnt; i++) if (gN[i].tick + gN[i].dur > total) inRange = false;
  check("每個音都在總長度內", inRange);

  // Player only ever scans forward, so notes must be sorted by increasing tick ——
  // once the cursor is past a note there is no going back and the rest of the piece
  // is silent. This is a hole we actually fell into: the first generator wrote voice
  // by voice, and only the bass of section A sounded.
  bool sorted = true;
  for (int i = 1; i < cnt; i++) if (gN[i].tick < gN[i - 1].tick) sorted = false;
  check("音符依 tick 遞增排序", sorted);

  // Duration must not be 0
  bool durOk = true;
  for (int i = 0; i < cnt; i++) if (gN[i].dur == 0) durOk = false;
  check("沒有長度為 0 的音", durOk);

  // No holes in the timeline (or you hear silence out of nowhere).
  // Only checked as far as "the end of the last note" —— the stretch after that is
  // deliberately left as tail for the release.
  int lastEnd = 0;
  for (int i = 0; i < cnt; i++) lastEnd = std::max(lastEnd, (int)(gN[i].tick + gN[i].dur));
  int gaps = 0, maxPoly = 0;
  for (int t = 0; t < lastEnd; t++) {
    const int p = polyAt(gN, cnt, t);
    if (p == 0) gaps++;
    maxPoly = std::max(maxPoly, p);
  }
  snprintf(msg, sizeof(msg), "(%d tick 沒有聲音；尾音留白 %d tick)", gaps, total - lastEnd);
  check("演奏區間沒有空洞", gaps == 0, msg);

  snprintf(msg, sizeof(msg), "(最多同時 %d 個，上限 %d)", maxPoly, TC_N_VOICES);
  check("同時發聲數不超過 TC_N_VOICES", maxPoly <= TC_N_VOICES, msg);

  // Voice distribution
  int part[4] = {0, 0, 0, 0};
  for (int i = 0; i < cnt; i++) if (gN[i].part < 4) part[gN[i].part]++;
  printf("     聲部分佈：旋律 %d  內聲部 %d  低音 %d\n", part[0], part[1], part[2]);
  printf("     長度：%.1f 秒 @ %.0f BPM\n",
         total * 60.0f / (TC_BPM * TC_TICKS_PER_BEAT), TC_BPM);
}

int main() {
  printf("\n樂譜驗證（音色庫涵蓋 C4~B4 = MIDI 60~71）\n");
  report();

  // The chromatic scale exists for note-by-note comparison, so absolutely no overlap
  const int cnt = buildScore(gN, TC_MAX_NOTES);
  int maxPoly = 0, lastEnd = 0;
  for (int i = 0; i < cnt; i++) lastEnd = std::max(lastEnd, (int)(gN[i].tick + gN[i].dur));
  for (int t = 0; t < lastEnd; t++)
    maxPoly = std::max(maxPoly, polyAt(gN, cnt, t));
  printf("\n=== 半音階的額外要求 ===\n");
  check("同一時間只有一個音（evaluate.py 才切得開）", maxPoly == 1);
  check("預設仍是 C3~B4 共 24 音（既有評測基準不能被改掉）", cnt == 24);
  check("預設第一個音是 C3(48)", cnt > 0 && gN[0].midi == 48);
  check("預設最後一個音是 B4(71)", cnt > 0 && gN[cnt - 1].midi == 71);

  // -------------------------------------------------------------------------
  //  Dynamic pitch range: in practice "the timbre bank's range, plus an octave up"
  // -------------------------------------------------------------------------
  printf("\n=== 動態音域 ===\n");
  {
    scoreSetScaleRange(60, 71 + 12);            // Sampled C4~B4 -> play C4~B5
    const int n2 = buildScore(gN, TC_MAX_NOTES);
    check("C4~B5 共 24 音", n2 == 24);
    check("從 C4(60) 開始", n2 > 0 && gN[0].midi == 60);
    check("到 B5(83) 結束", n2 > 0 && gN[n2 - 1].midi == 83);

    // Notes must run consecutively upward with no overlap, or evaluate.py can't line them up
    bool mono = true;
    for (int i = 1; i < n2; i++)
      if (gN[i].midi != gN[i - 1].midi + 1 || gN[i].tick <= gN[i - 1].tick) mono = false;
    check("半音連續遞增且時間不重疊", mono);

    // Total length has to follow the note count, or Player stops early or spins idle
    const uint16_t tot = scoreTotalTicks();
    check("總長度涵蓋最後一個音", tot >= gN[n2 - 1].tick + gN[n2 - 1].dur);

    // The name is what the panel shows: if the range changes the name must change too
    // —— a display that disagrees with reality is the hardest thing to track down
    check("名稱跟著音域走", strcmp(scoreName(), "C4-B5 scale") == 0);

    // Edge case: a reversed range must correct itself, not yield a negative note count or overrun the array
    scoreSetScaleRange(80, 60);
    const int n3 = buildScore(gN, TC_MAX_NOTES);
    check("上下顛倒會自動對調", n3 == 21 && gN[0].midi == 60);

    // The timbre bank could in principle span a very wide range; adding an octave must not overflow the array
    scoreSetScaleRange(0, 127);
    const int n4 = buildScore(gN, TC_MAX_NOTES);
    check("極端音域不會寫爆 TC_MAX_NOTES", n4 > 0 && n4 <= TC_MAX_NOTES);

    scoreSetScaleRange(48, 71);                 // Restore, so nothing later is affected
  }

  // -------------------------------------------------------------------------
  //  Canon
  //
  //  The chromatic scale tests "it comes apart cleanly"; the canon has to test
  //  three entirely different things:
  //    1) three voices stacked up don't blow past the voice count (TC_N_VOICES)
  //    2) after octave placement every note is still inside "the range the timbre
  //       bank covers + one octave"
  //       —— this is the only logic newly written when this score was wired back
  //       in, and the easiest place to get it wrong
  //    3) the bass hasn't flipped above the melody
  // -------------------------------------------------------------------------
  auto canonCheck = [&](const char *what, int bankLo, int bankHi) {
    scoreSetScaleRange(bankLo, bankHi + 12);
    scoreSetMode(TC_SCORE_CANON);
    const int n = buildScore(gN, TC_MAX_NOTES);
    const int winLo = bankLo, winHi = bankHi + 12;
    char msg[96];

    printf("\n--- %s（音色庫 %d~%d，可用窗 %d~%d）---\n", what, bankLo, bankHi, winLo, winHi);
    // 112 notes in the original score; merging unisons loses a few (when the bass is
    // placed into an inner voice's range part of it ends up overlapping), and losing
    // too many means transposition has collapsed a whole line
    snprintf(msg, sizeof(msg), "(%d 個音符，原譜 112，合併掉 %d)", n, 112 - n);
    check("卡農有產生音符（合併後仍在合理範圍）", n >= 80 && n <= 112, msg);

    // Player keys note-off by pitch: when two notes of the same pitch overlap, the one
    // that ends first shuts off the one still sounding. The symptom of this rule
    // breaking is the bass cutting out for half a beat in a few bars —— only this test
    // catches it, the ear can't find it.
    int unison = 0;
    for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++) {
        if (gN[i].midi != gN[j].midi) continue;
        if (gN[j].tick >= gN[i].tick + gN[i].dur) continue;
        if (gN[i].tick >= gN[j].tick + gN[j].dur) continue;
        unison++;
      }
    snprintf(msg, sizeof(msg), "(%d 對)", unison);
    check("沒有兩個音同時同音高", unison == 0, msg);

    // Every note must land inside the usable window. Falling outside means the octave
    // placement was computed wrong —— it sounds like "one of the lines gone dull or
    // gone shrill", and the ear has a hard time saying where it went wrong.
    int lo = 127, hi = 0, outside = 0;
    for (int i = 0; i < n; i++) {
      lo = std::min(lo, (int)gN[i].midi);
      hi = std::max(hi, (int)gN[i].midi);
      if (gN[i].midi < winLo || gN[i].midi > winHi) outside++;
    }
    snprintf(msg, sizeof(msg), "(實際 %d~%d，出界 %d 個)", lo, hi, outside);
    check("每個音都在可用窗內", outside == 0, msg);

    // Octave shifts only -> the key is unchanged. Scale degrees of D major: C# D E F# G A B
    const bool kInKey[12] = { false, true, true, false, true, false,
                              true, true, false, true, false, true };
    bool keyOk = true;
    for (int i = 0; i < n; i++) if (!kInKey[gN[i].midi % 12]) keyOk = false;
    check("移調只動八度，仍然是 D 大調", keyOk);

    // The shift is always a multiple of 12, otherwise someone wrote "transpose" as "rewrite the melody"
    bool oct = true;
    for (int p2 = 0; p2 < 3; p2++) if (scoreCanonShift(p2) % 12 != 0) oct = false;
    snprintf(msg, sizeof(msg), "(旋律 %+d 內聲部 %+d 低音 %+d)",
             scoreCanonShift(0), scoreCanonShift(1), scoreCanonShift(2));
    check("位移量都是整個八度", oct, msg);

    // Voice order: judged by mean pitch (the lines overlap by nature, so it is steadier than highest/lowest note)
    double sum[3] = {0, 0, 0};
    int    cn[3]  = {0, 0, 0};
    for (int i = 0; i < n; i++) if (gN[i].part < 3) { sum[gN[i].part] += gN[i].midi; cn[gN[i].part]++; }
    const double mMel = cn[0] ? sum[0] / cn[0] : 0, mIn = cn[1] ? sum[1] / cn[1] : 0,
                 mBass = cn[2] ? sum[2] / cn[2] : 0;
    snprintf(msg, sizeof(msg), "(低音 %.1f <= 內聲部 %.1f <= 旋律 %.1f)", mBass, mIn, mMel);
    check("低音沒有翻到旋律上面", mBass <= mIn && mIn <= mMel, msg);

    // Simultaneous voices: three parts + release tails, never over the synth's voice count
    int lastEnd = 0, maxPoly = 0, gaps = 0;
    for (int i = 0; i < n; i++) lastEnd = std::max(lastEnd, (int)(gN[i].tick + gN[i].dur));
    for (int t = 0; t < lastEnd; t++) {
      const int poly = polyAt(gN, n, t);
      maxPoly = std::max(maxPoly, poly);
      if (poly == 0) gaps++;
    }
    snprintf(msg, sizeof(msg), "(最多同時 %d 個，上限 %d)", maxPoly, TC_N_VOICES);
    check("同時發聲數不超過 TC_N_VOICES", maxPoly <= TC_N_VOICES, msg);
    check("演奏區間沒有空洞", gaps == 0);

    bool sorted = true, durOk = true;
    for (int i = 1; i < n; i++) if (gN[i].tick < gN[i - 1].tick) sorted = false;
    for (int i = 0; i < n; i++) if (gN[i].dur == 0) durOk = false;
    check("音符依 tick 遞增排序", sorted);
    check("沒有長度為 0 的音", durOk);

    const uint16_t tot = scoreTotalTicks();
    snprintf(msg, sizeof(msg), "(總長 %u tick，最後一個音結束於 %d)", tot, lastEnd);
    check("總長度涵蓋最後一個音", tot >= lastEnd, msg);
    printf("     %s，%.1f 秒 @ %.0f BPM\n", scoreName(),
           tot * 60.0f / (TC_BPM * TC_TICKS_PER_BEAT), TC_BPM);

    scoreSetMode(TC_SCORE_SCALE);
  };

  printf("\n=== 卡農 ===\n");
  // The most common case today: Iowa MIS samples, C4~B4
  canonCheck("音色庫在中央", 60, 71);
  // The trumpet set: E3~B5, a wide window, none of the three lines should need moving
  canonCheck("音色庫很寬", 52, 83);
  // Extreme: the whole timbre bank sits in the low register. This one forces out the
  // "voice flipped over" guard —— the inner voice and the melody get pushed down,
  // while the bass is already down there and doesn't move
  canonCheck("音色庫很低", 36, 47);
  // Extreme: the whole timbre bank sits in the high register
  canonCheck("音色庫很高", 84, 95);

  printf("\n=== 模式切換 ===\n");
  {
    scoreSetScaleRange(60, 71 + 12);
    scoreSetMode(TC_SCORE_CANON);
    const int nc = buildScore(gN, TC_MAX_NOTES);
    scoreSetMode(TC_SCORE_SCALE);
    const int ns = buildScore(gN, TC_MAX_NOTES);
    char m2[64];
    snprintf(m2, sizeof(m2), "(卡農 %d 音 -> 半音階 %d 音)", nc, ns);
    check("切回半音階會拿到半音階", ns == 24 && nc > 50, m2);
    check("預設模式是半音階（評測基準的前提）", scoreGetMode() == TC_SCORE_SCALE);
    check("半音階的名稱沒被卡農汙染", strcmp(scoreName(), "C4-B5 scale") == 0);
    scoreSetScaleRange(48, 71);
  }

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

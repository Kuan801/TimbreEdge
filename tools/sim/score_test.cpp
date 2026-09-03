// ============================================================================
//  score_test.cpp  -  在桌機上驗證兩份樂譜（半音階與卡農）
//
//  用法：  make score_test && ./score_test
//
//  樂譜是純資料產生器，不用 Arduino 也不用音訊。手寫 100 多個音符很容易
//  出現「重疊到超過聲部數」「某個 tick 空掉」「音高跑出音色庫太遠」這種錯，
//  燒進去用耳朵聽反而不容易定位。
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

// 某個 tick 同時有幾個音在響
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

  // 音域
  int lo = 127, hi = 0;
  for (int i = 0; i < cnt; i++) { lo = std::min(lo, (int)gN[i].midi); hi = std::max(hi, (int)gN[i].midi); }
  snprintf(msg, sizeof(msg), "(%d ~ %d)", lo, hi);
  check("音高都在 MIDI 合法範圍", lo >= 0 && hi <= 127, msg);

  // 音色庫只有 C4~B4 (60~71)，離太遠移調品質會掉
  const int down = 60 - lo, up = hi - 71;
  snprintf(msg, sizeof(msg), "(往下 %d、往上 %d 個半音)", down > 0 ? down : 0, up > 0 ? up : 0);
  check("移調距離不超過 12 個半音", (down <= 12) && (up <= 12), msg);

  // 每個音都要落在總長度內
  bool inRange = true;
  for (int i = 0; i < cnt; i++) if (gN[i].tick + gN[i].dur > total) inRange = false;
  check("每個音都在總長度內", inRange);

  // Player 是一路往前掃的，音符必須依 tick 遞增排好 —— 沒排序的話游標
  // 越過某個音之後就再也回不去，後面整段靜音。這是實際踩過的坑：
  // 第一版產生器照聲部書寫，結果只有 A 段的低音在響。
  bool sorted = true;
  for (int i = 1; i < cnt; i++) if (gN[i].tick < gN[i - 1].tick) sorted = false;
  check("音符依 tick 遞增排序", sorted);

  // 音長不能是 0
  bool durOk = true;
  for (int i = 0; i < cnt; i++) if (gN[i].dur == 0) durOk = false;
  check("沒有長度為 0 的音", durOk);

  // 時間軸不能有空洞（否則會聽到莫名的靜音）。
  // 只檢查到「最後一個音結束」為止 —— 之後那段是刻意留給 release 的尾巴。
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

  // 聲部分佈
  int part[4] = {0, 0, 0, 0};
  for (int i = 0; i < cnt; i++) if (gN[i].part < 4) part[gN[i].part]++;
  printf("     聲部分佈：旋律 %d  內聲部 %d  低音 %d\n", part[0], part[1], part[2]);
  printf("     長度：%.1f 秒 @ %.0f BPM\n",
         total * 60.0f / (TC_BPM * TC_TICKS_PER_BEAT), TC_BPM);
}

int main() {
  printf("\n樂譜驗證（音色庫涵蓋 C4~B4 = MIDI 60~71）\n");
  report();

  // 半音階是拿來逐音比對的，絕對不能有重疊
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
  //  動態音域：實務上是「音色庫的音域，再往上一個八度」
  // -------------------------------------------------------------------------
  printf("\n=== 動態音域 ===\n");
  {
    scoreSetScaleRange(60, 71 + 12);            // 採到 C4~B4 -> 演奏 C4~B5
    const int n2 = buildScore(gN, TC_MAX_NOTES);
    check("C4~B5 共 24 音", n2 == 24);
    check("從 C4(60) 開始", n2 > 0 && gN[0].midi == 60);
    check("到 B5(83) 結束", n2 > 0 && gN[n2 - 1].midi == 83);

    // 音符必須連續遞增、不重疊，否則 evaluate.py 對不上
    bool mono = true;
    for (int i = 1; i < n2; i++)
      if (gN[i].midi != gN[i - 1].midi + 1 || gN[i].tick <= gN[i - 1].tick) mono = false;
    check("半音連續遞增且時間不重疊", mono);

    // 總長度要跟著音數走，否則 Player 會提早停或空轉
    const uint16_t tot = scoreTotalTicks();
    check("總長度涵蓋最後一個音", tot >= gN[n2 - 1].tick + gN[n2 - 1].dur);

    // 名稱是給面板看的，音域變了名稱也要變 —— 顯示跟實際不符最難查
    check("名稱跟著音域走", strcmp(scoreName(), "C4-B5 scale") == 0);

    // 邊界：顛倒的範圍要能自己修正，不能產生負數音數或寫爆陣列
    scoreSetScaleRange(80, 60);
    const int n3 = buildScore(gN, TC_MAX_NOTES);
    check("上下顛倒會自動對調", n3 == 21 && gN[0].midi == 60);

    // 音色庫理論上可以涵蓋很寬的音域，加一個八度後不能超過陣列
    scoreSetScaleRange(0, 127);
    const int n4 = buildScore(gN, TC_MAX_NOTES);
    check("極端音域不會寫爆 TC_MAX_NOTES", n4 > 0 && n4 <= TC_MAX_NOTES);

    scoreSetScaleRange(48, 71);                 // 還原，不影響後面
  }

  // -------------------------------------------------------------------------
  //  卡農
  //
  //  半音階驗的是「切得開」，卡農要驗的是完全不同的三件事：
  //    1) 三聲部疊起來不會爆掉聲部數（TC_N_VOICES）
  //    2) 八度安置之後每個音都還在「音色庫涵蓋範圍 + 一個八度」裡面
  //       —— 這正是接回這份譜時唯一新寫的邏輯，也是最容易錯的地方
  //    3) 低音沒有翻到旋律上面去
  // -------------------------------------------------------------------------
  auto canonCheck = [&](const char *what, int bankLo, int bankHi) {
    scoreSetScaleRange(bankLo, bankHi + 12);
    scoreSetMode(TC_SCORE_CANON);
    const int n = buildScore(gN, TC_MAX_NOTES);
    const int winLo = bankLo, winHi = bankHi + 12;
    char msg[96];

    printf("\n--- %s（音色庫 %d~%d，可用窗 %d~%d）---\n", what, bankLo, bankHi, winLo, winHi);
    // 原譜 112 個音符；同音合併之後會少一些（低音被安置到內聲部的音域時
    // 會有一部分重疊在一起），少太多就代表移調把整條線疊掉了
    snprintf(msg, sizeof(msg), "(%d 個音符，原譜 112，合併掉 %d)", n, 112 - n);
    check("卡農有產生音符（合併後仍在合理範圍）", n >= 80 && n <= 112, msg);

    // Player 的 note-off 是用音高當鍵的：兩個同音高的音疊在一起時，
    // 先結束的會把還在響的一起關掉。這條規則壞掉的症狀是某幾個小節
    // 低音突然斷半拍 —— 只能用這個測試抓，耳朵找不到。
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

    // 音都要落在可用窗內。掉出去代表八度安置算錯了 ——
    // 聽起來就是「某一條線悶掉或尖掉」，用耳朵很難指認是哪裡出錯。
    int lo = 127, hi = 0, outside = 0;
    for (int i = 0; i < n; i++) {
      lo = std::min(lo, (int)gN[i].midi);
      hi = std::max(hi, (int)gN[i].midi);
      if (gN[i].midi < winLo || gN[i].midi > winHi) outside++;
    }
    snprintf(msg, sizeof(msg), "(實際 %d~%d，出界 %d 個)", lo, hi, outside);
    check("每個音都在可用窗內", outside == 0, msg);

    // 只移八度 -> 調性不變。D 大調的音級：C# D E F# G A B
    const bool kInKey[12] = { false, true, true, false, true, false,
                              true, true, false, true, false, true };
    bool keyOk = true;
    for (int i = 0; i < n; i++) if (!kInKey[gN[i].midi % 12]) keyOk = false;
    check("移調只動八度，仍然是 D 大調", keyOk);

    // 位移量一定是 12 的倍數，否則就是有人把「移調」寫成了「改旋律」
    bool oct = true;
    for (int p2 = 0; p2 < 3; p2++) if (scoreCanonShift(p2) % 12 != 0) oct = false;
    snprintf(msg, sizeof(msg), "(旋律 %+d 內聲部 %+d 低音 %+d)",
             scoreCanonShift(0), scoreCanonShift(1), scoreCanonShift(2));
    check("位移量都是整個八度", oct, msg);

    // 聲部順序：用平均音高判斷（三條線本來就交疊，比最高/最低音穩定）
    double sum[3] = {0, 0, 0};
    int    cn[3]  = {0, 0, 0};
    for (int i = 0; i < n; i++) if (gN[i].part < 3) { sum[gN[i].part] += gN[i].midi; cn[gN[i].part]++; }
    const double mMel = cn[0] ? sum[0] / cn[0] : 0, mIn = cn[1] ? sum[1] / cn[1] : 0,
                 mBass = cn[2] ? sum[2] / cn[2] : 0;
    snprintf(msg, sizeof(msg), "(低音 %.1f <= 內聲部 %.1f <= 旋律 %.1f)", mBass, mIn, mMel);
    check("低音沒有翻到旋律上面", mBass <= mIn && mIn <= mMel, msg);

    // 同時發聲數：三聲部 + release 尾音，絕對不能超過合成器的聲部數
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
  // 現況最常見的情形：Iowa MIS 素材 C4~B4
  canonCheck("音色庫在中央", 60, 71);
  // 小號那批：E3~B5，窗很寬，三條線應該都不用搬
  canonCheck("音色庫很寬", 52, 83);
  // 極端：音色庫整個在低音域。這一組會逼出「聲部翻過去」的保護 ——
  // 內聲部與旋律被往下推，而低音本來就在下面不用動
  canonCheck("音色庫很低", 36, 47);
  // 極端：音色庫整個在高音域
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

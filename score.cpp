#include "score.h"
#include <stdlib.h>   // abs

// ============================================================================
//  兩份樂譜
//
//  半音階  不是拿來聽的，是拿來量的：每個音獨立、不重疊，tools/evaluate.py
//          才能自動切開並跟對應的素材算包絡相關性與頻譜距離。
//  卡農    不是拿來量的，是拿來聽的：三聲部同時發聲，用來判斷這個音色
//          拿去演奏音樂像不像。逐音比對對它沒有意義。
//
//  卡農曾經被整份移除（理由就是「沒辦法逐音比對」），舊碼留在
//  舊版備份/score_canon.cpp。接回來的版本多了一件事：音高不再寫死，
//  會依音色庫實際涵蓋的音域逐聲部做八度安置 —— 見下面 pickOctaveShift()。
// ============================================================================

#define TICKS_PER_BAR   16      // TC_TICKS_PER_BEAT = 4，4/4 拍
#define HALF            8
#define QUARTER         4
#define EIGHTH          2

// ---------------------------------------------------------------------------
//  目前要產生哪一份譜
//
//  預設一定是半音階。evaluate.py / bench.py / score_test 全部建立在它上面，
//  「開機後預設是別的譜」會讓既有的評測基準悄悄失效，而且不會有任何報錯。
// ---------------------------------------------------------------------------
static ScoreMode gMode = TC_SCORE_SCALE;

void      scoreSetMode(ScoreMode m) { gMode = m; }
ScoreMode scoreGetMode()            { return gMode; }

// ---------------------------------------------------------------------------
//  半音階
// ---------------------------------------------------------------------------
#define SCALE_TICKS     HALF

// 預設 C3~B4（48~71），跟改版前一致，評測腳本與 score_test 才不會受影響
static int gScaleLo = 48;
static int gScaleHi = 71;

void scoreSetScaleRange(int midiLo, int midiHi) {
  if (midiLo < 0)   midiLo = 0;
  if (midiHi > 127) midiHi = 127;
  if (midiHi < midiLo) { const int t = midiLo; midiLo = midiHi; midiHi = t; }

  // 上限：整首要塞得進 Player 的音符陣列。半音階是單聲部，一個半音一個音符，
  // 所以直接夾住音數就好。TC_MAX_NOTES 是 224，實際上碰不到，
  // 但音色庫理論上可以涵蓋很寬的音域，還是擋一下。
  if (midiHi - midiLo + 1 > TC_MAX_NOTES) midiHi = midiLo + TC_MAX_NOTES - 1;

  gScaleLo = midiLo;
  gScaleHi = midiHi;
}

void scoreGetScaleRange(int *midiLo, int *midiHi) {
  if (midiLo) *midiLo = gScaleLo;
  if (midiHi) *midiHi = gScaleHi;
}

static int scaleNoteCount() { return gScaleHi - gScaleLo + 1; }

static int buildScale(ScoreNote *notes, int maxNotes) {
  int c = 0;
  const int n = scaleNoteCount();
  for (int i = 0; i < n && c < maxNotes; i++) {
    notes[c].tick = (uint16_t)(i * SCALE_TICKS);
    notes[c].dur  = SCALE_TICKS;
    notes[c].midi = (uint8_t)(gScaleLo + i);
    notes[c].vel  = 100;
    notes[c].part = 0;
    c++;
  }
  return c;
}

// ===========================================================================
//  卡農（Canon in D，簡化成三聲部）
//
//  和聲進行（每個和弦一個二分音符，8 個和弦 = 4 小節，重複三輪）：
//        D    A    Bm   F#m  G    D    G    A
//
//  三輪刻意寫成三種織度：A 段先把和聲聽清楚、B 段把主題切成四分音符、
//  C 段是八分音符琶音。同一個音色在「長音」與「快速音群」下的差別，
//  是這份譜真正想聽出來的東西 —— 起音與釋放的品質只有在 C 段才聽得出來。
// ---------------------------------------------------------------------------
static const uint8_t kBass[8]  = { 50, 57, 59, 54, 55, 50, 55, 57 };
//                                 D3  A3  B3  F#3 G3  D3  G3  A3

// A 段主題（二分音符）：F#5 E5 D5 C#5 B4 A4 B4 C#5
static const uint8_t kMelA[8]  = { 78, 76, 74, 73, 71, 69, 71, 73 };

// 內聲部：主題下方的三度/六度
static const uint8_t kInner[8] = { 74, 73, 71, 69, 67, 66, 67, 69 };
//                                 D5  C#5 B4  A4  G4  F#4 G4  A4

// 每個和弦的三和音，C 段的琶音用
static const uint8_t kChord[8][3] = {
  { 62, 66, 69 },   // D   D  F# A
  { 69, 73, 76 },   // A   A  C# E
  { 71, 74, 78 },   // Bm  B  D  F#
  { 66, 69, 73 },   // F#m F# A  C#
  { 67, 71, 74 },   // G   G  B  D
  { 62, 66, 69 },   // D
  { 67, 71, 74 },   // G
  { 69, 73, 76 },   // A
};

#define ROUND_TICKS   (8 * HALF)      // 一輪 = 8 個二分音符 = 64 tick
#define CANON_ROUNDS  3
#define CANON_TICKS   (CANON_ROUNDS * ROUND_TICKS)
#define CANON_PARTS   3

// 上一次產生時每個聲部實際移了幾個半音（12 的倍數），給序列埠印用
static int gCanonShift[CANON_PARTS] = { 0, 0, 0 };

int scoreCanonShift(int part) {
  return (part >= 0 && part < CANON_PARTS) ? gCanonShift[part] : 0;
}

static inline void put(ScoreNote *n, int &c, int maxNotes,
                       uint16_t tick, uint16_t dur, uint8_t midi,
                       uint8_t vel, uint8_t part) {
  if (c >= maxNotes) return;
  n[c].tick = tick; n[c].dur = dur; n[c].midi = midi;
  n[c].vel = vel;   n[c].part = part;
  c++;
}

// 原調（D 大調，低音 D3、旋律到 F#5）。移調在後面統一做，
// 產生器本身維持「照譜寫」的樣子，改旋律時不用同時想移調的事。
static int buildCanonRaw(ScoreNote *notes, int maxNotes) {
  int c = 0;

  for (int r = 0; r < CANON_ROUNDS; r++) {
    const uint16_t base = (uint16_t)(r * ROUND_TICKS);

    // ---- 低音：三輪都一樣，二分音符 ----------------------------------
    for (int i = 0; i < 8; i++)
      put(notes, c, maxNotes, (uint16_t)(base + i * HALF), HALF, kBass[i], 88, 2);

    if (r == 0) {
      // ---- A 段：主題與內聲部都用二分音符，先把和聲聽清楚 -----------
      for (int i = 0; i < 8; i++) {
        put(notes, c, maxNotes, (uint16_t)(base + i * HALF), HALF, kMelA[i],  105, 0);
        put(notes, c, maxNotes, (uint16_t)(base + i * HALF), HALF, kInner[i],  78, 1);
      }

    } else if (r == 1) {
      // ---- B 段：主題拆成四分音符，加一條下行對句 --------------------
      for (int i = 0; i < 8; i++) {
        const uint16_t t = (uint16_t)(base + i * HALF);
        put(notes, c, maxNotes, t,           QUARTER, kMelA[i], 105, 0);
        // 後半拍走到下一個和弦的音，線條才會流動而不是原地踏步
        put(notes, c, maxNotes, (uint16_t)(t + QUARTER), QUARTER,
            kMelA[(i + 1) % 8], 98, 0);
        // 內聲部維持二分音符當襯底
        put(notes, c, maxNotes, t, HALF, kInner[i], 74, 1);
      }

    } else {
      // ---- C 段：八分音符琶音，卡農最有名的流動線條 ------------------
      for (int i = 0; i < 8; i++) {
        const uint16_t t = (uint16_t)(base + i * HALF);
        const uint8_t *ch = kChord[i];
        // 根 -> 三 -> 五 -> 三，四個八分音符剛好填滿一個二分音符
        put(notes, c, maxNotes, (uint16_t)(t + 0 * EIGHTH), EIGHTH, ch[0], 100, 0);
        put(notes, c, maxNotes, (uint16_t)(t + 1 * EIGHTH), EIGHTH, ch[1],  96, 0);
        put(notes, c, maxNotes, (uint16_t)(t + 2 * EIGHTH), EIGHTH, ch[2], 100, 0);
        put(notes, c, maxNotes, (uint16_t)(t + 3 * EIGHTH), EIGHTH, ch[1],  96, 0);
        // 內聲部用四分音符，不要跟琶音打架
        put(notes, c, maxNotes, t, QUARTER, kInner[i], 72, 1);
        put(notes, c, maxNotes, (uint16_t)(t + QUARTER), QUARTER,
            kInner[(i + 1) % 8], 70, 1);
      }
    }
  }
  return c;
}

// ---------------------------------------------------------------------------
//  八度安置：讓卡農落在這台機器「驗證過」的音域裡
//
//  為什麼需要：原譜是 D 大調、低音到 D3(50)。音色庫常常只涵蓋 C4~B4，
//  照原調演奏的話低音每個音都要往下移調 10 個半音以上 —— 聽起來不像，
//  而且很容易被誤會成合成器壞掉（README 的「半音階以前寫死 C3~B4」
//  就是同一個坑，那次的結論是：音域要跟著音色庫走）。
//
//  目標音域用的是跟半音階同一組 [gScaleLo, gScaleHi]，也就是
//  「音色庫涵蓋的範圍，再往上一個八度」。
//
//  三個設計決定：
//
//  1) 只移整個八度。移 12 的倍數不會改調性，卡農還是 D 大調；
//     移別的量會變成「換一首曲子」，那不是這裡該做的事。
//
//  2) 逐聲部各自選八度，不是整首一起移。原譜跨了 29 個半音（D3~F#5），
//     而音色庫 C4~B4 的可用窗只有 24 個 —— 整首一起移一定有一端出界。
//     逐聲部選則三條線各自都塞得進去，而且每條線內部的音程完全不變
//     （不會出現「某幾個音突然跳一個八度」那種破壞旋律的折疊）。
//     密集位置本來就是常見的簡化編法，音樂上不會奇怪。
//
//  3) 成本函數裡「往下出界」的權重是「往上」的兩倍。往下移調要憑空生出
//     比素材更低的基頻，往上只是既有諧波的重新配置 —— 這在 README
//     的移調討論裡已經量過，兩者不對等。
// ---------------------------------------------------------------------------
static int pickOctaveShift(int pmin, int pmax) {
  int best = 0, bestCost = 1 << 30;
  for (int k = -36; k <= 36; k += 12) {
    const int lo = pmin + k, hi = pmax + k;
    if (lo < 0 || hi > 127) continue;
    const int below = (gScaleLo > lo) ? (gScaleLo - lo) : 0;
    const int above = (hi > gScaleHi) ? (hi - gScaleHi) : 0;
    const int cost  = below * 2 + above;
    // 同分時選移動量最小的：沒有理由多搬一個八度
    if (cost < bestCost || (cost == bestCost && abs(k) < abs(best))) {
      bestCost = cost;
      best = k;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
//  同一瞬間不能有兩個一樣的音高
//
//  這是 Player 的限制，不是音樂上的講究：note-off 是用「音高」當鍵的
//  （_pend 裡存的就是 midi），所以兩個同音高的音疊在一起時，先結束的那一個
//  會把還在響的那一個一起關掉。實際聽到的症狀是「低音莫名其妙斷掉半拍」，
//  而且只在某幾個小節出現 —— 用耳朵幾乎不可能定位。
//
//  卡農為什麼會撞到：音色庫只有一個八度時，低音被往上安置一個八度之後
//  就跟內聲部踩進同一個音域，8 個和弦裡有 4 個會出現同音。原本寫死音高的
//  版本不會遇到（低音固定在下面一個八度），是「跟著音色庫走」帶出來的新情況。
//
//  處理方式是合併成一個音，而不是丟掉其中一個：時間取聯集、力度取大的，
//  聲部取「音比較長的那一條」（長的通常是襯底，pan 才不會忽左忽右）。
//  聽感上本來就只會聽到一個音，合併之後只是少了一次無謂的重疊。
// ---------------------------------------------------------------------------
static int mergeUnisons(ScoreNote *n, int cnt) {
  for (int i = 0; i < cnt; i++) {
    for (int j = i + 1; j < cnt; j++) {
      if (n[i].midi != n[j].midi) continue;
      const int ai = n[i].tick, bi = ai + n[i].dur;
      const int aj = n[j].tick, bj = aj + n[j].dur;
      if (aj >= bi || ai >= bj) continue;              // 沒有重疊，兩個音各自安好

      const int st = (ai < aj) ? ai : aj;
      const int en = (bi > bj) ? bi : bj;
      const uint8_t part = (n[j].dur > n[i].dur) ? n[j].part
                         : (n[i].dur > n[j].dur) ? n[i].part
                         : (n[i].part < n[j].part ? n[i].part : n[j].part);
      n[i].tick = (uint16_t)st;
      n[i].dur  = (uint16_t)(en - st);
      if (n[j].vel > n[i].vel) n[i].vel = n[j].vel;
      n[i].part = part;

      for (int k = j; k < cnt - 1; k++) n[k] = n[k + 1];
      cnt--;
      j--;                                             // j 位置換人了，重驗一次
    }
  }
  return cnt;
}

static int buildCanon(ScoreNote *notes, int maxNotes) {
  const int cnt = buildCanonRaw(notes, maxNotes);
  if (cnt <= 0) return cnt;

  // ---- 各聲部的原調音域與平均音高 ----------------------------------------
  int pmin[CANON_PARTS], pmax[CANON_PARTS], pcnt[CANON_PARTS];
  long psum[CANON_PARTS];
  for (int p = 0; p < CANON_PARTS; p++) { pmin[p] = 127; pmax[p] = 0; pcnt[p] = 0; psum[p] = 0; }
  for (int i = 0; i < cnt; i++) {
    const int p = notes[i].part;
    if (p < 0 || p >= CANON_PARTS) continue;
    const int m = notes[i].midi;
    if (m < pmin[p]) pmin[p] = m;
    if (m > pmax[p]) pmax[p] = m;
    psum[p] += m;
    pcnt[p]++;
  }

  int shift[CANON_PARTS] = { 0, 0, 0 };
  for (int p = 0; p < CANON_PARTS; p++)
    if (pcnt[p]) shift[p] = pickOctaveShift(pmin[p], pmax[p]);

  // ---- 聲部不能翻過去 -----------------------------------------------------
  //
  // 逐聲部各自挑八度有一個副作用：音色庫很低的時候，內聲部與旋律會被
  // 一路往下推，而低音本來就在下面、不用動 —— 結果低音跑到旋律上面去，
  // 聽起來就不是卡農了。這裡把「低音 <= 內聲部 <= 旋律」當成硬條件，
  // 用平均音高判斷（三條線本來就交疊，用最高/最低音判斷會過度敏感）。
  const float base0 = pcnt[0] ? (float)psum[0] / pcnt[0] : 0.0f;
  const float base1 = pcnt[1] ? (float)psum[1] / pcnt[1] : 0.0f;
  const float base2 = pcnt[2] ? (float)psum[2] / pcnt[2] : 0.0f;
  for (int guard = 0; guard < CANON_PARTS; guard++) {
    bool changed = false;
    if (pcnt[1] && pcnt[0] && base1 + shift[1] > base0 + shift[0] && pmin[1] + shift[1] - 12 >= 0) {
      shift[1] -= 12; changed = true;
    }
    if (pcnt[2] && pcnt[1] && base2 + shift[2] > base1 + shift[1] && pmin[2] + shift[2] - 12 >= 0) {
      shift[2] -= 12; changed = true;
    }
    if (!changed) break;
  }

  // ---- 套用 ---------------------------------------------------------------
  for (int i = 0; i < cnt; i++) {
    const int p = notes[i].part;
    if (p < 0 || p >= CANON_PARTS) continue;
    int m = (int)notes[i].midi + shift[p];
    // 保險：任何情況下都不能送出界外的 MIDI 音高（用八度折回來，
    // 不要 clamp —— clamp 會把好幾個音壓成同一個，聽起來像卡住）
    while (m < 0)   m += 12;
    while (m > 127) m -= 12;
    notes[i].midi = (uint8_t)m;
  }
  for (int p = 0; p < CANON_PARTS; p++) gCanonShift[p] = shift[p];

  // 移調完才會知道有沒有撞到同音，所以合併放在最後
  return mergeUnisons(notes, cnt);
}

// ---------------------------------------------------------------------------
//  Player 是「一路往前掃」的：fireTick() 只在 _notes[_cursor].tick == _tick
//  時才前進。所以音符必須依 tick 由小到大排好，否則游標一旦越過某個音就
//  再也回不去，後面全部靜音。
//
//  上面的產生器是照「聲部」寫的（低音整輪、再旋律整輪），可讀性好但順序不對
//  —— 第一次跑出來就是只有 A 段的低音在響、之後 30 秒全靜音。
//  在這裡統一排序，產生器就可以維持照聲部書寫。
//
//  插入排序：112 個音符跑一次不到 0.1 ms，而且已經接近有序（每個聲部內部
//  本來就遞增），實際比較次數遠低於最壞情況。
static void sortByTick(ScoreNote *n, int cnt) {
  for (int i = 1; i < cnt; i++) {
    ScoreNote key = n[i];
    int j = i - 1;
    while (j >= 0 && n[j].tick > key.tick) { n[j + 1] = n[j]; j--; }
    n[j + 1] = key;
  }
}

int buildScore(ScoreNote *notes, int maxNotes) {
  const int c = (gMode == TC_SCORE_CANON) ? buildCanon(notes, maxNotes)
                                          : buildScale(notes, maxNotes);
  sortByTick(notes, c);
  return c;
}

uint16_t scoreTotalTicks() {
  if (gMode == TC_SCORE_CANON) {
    // 尾端留一個二分音符：C 段最後一個琶音的 release 還在響，
    // 提早停會把尾音硬切掉，錄成 WAV 之後那一刀特別明顯
    return (uint16_t)(CANON_TICKS + HALF);
  }
  // 尾端多留一個 SCALE_TICKS 當釋放尾巴，最後一個音才不會被硬切
  return (uint16_t)(scaleNoteCount() * SCALE_TICKS + SCALE_TICKS);
}

const char *scoreName() {
  // 音域是動態的，名稱也要跟著動，否則面板會顯示跟實際不符的音域
  static const char *kNames[12] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
  static char buf[24];

  if (gMode == TC_SCORE_CANON) {
    // 只移八度所以調性不變，但旋律被放到哪個八度會影響聽感，寫進名稱裡
    snprintf(buf, sizeof(buf), "Canon in D %+d", gCanonShift[0]);
    return buf;
  }

  snprintf(buf, sizeof(buf), "%s%d-%s%d scale",
           kNames[((gScaleLo % 12) + 12) % 12], gScaleLo / 12 - 1,
           kNames[((gScaleHi % 12) + 12) % 12], gScaleHi / 12 - 1);
  return buf;
}

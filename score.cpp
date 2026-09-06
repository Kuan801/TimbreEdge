#include "score.h"
#include <stdlib.h>

#define TICKS_PER_BAR   16
#define HALF            8
#define QUARTER         4
#define EIGHTH          2

static ScoreMode gMode = TC_SCORE_SCALE;

void      scoreSetMode(ScoreMode m) { gMode = m; }
ScoreMode scoreGetMode()            { return gMode; }

#define SCALE_TICKS     HALF

static int gScaleLo = 48;
static int gScaleHi = 71;

void scoreSetScaleRange(int midiLo, int midiHi) {
  if (midiLo < 0)   midiLo = 0;
  if (midiHi > 127) midiHi = 127;
  if (midiHi < midiLo) { const int t = midiLo; midiLo = midiHi; midiHi = t; }

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

static const uint8_t kBass[8]  = { 50, 57, 59, 54, 55, 50, 55, 57 };

static const uint8_t kMelA[8]  = { 78, 76, 74, 73, 71, 69, 71, 73 };

static const uint8_t kInner[8] = { 74, 73, 71, 69, 67, 66, 67, 69 };

static const uint8_t kChord[8][3] = {
  { 62, 66, 69 },
  { 69, 73, 76 },
  { 71, 74, 78 },
  { 66, 69, 73 },
  { 67, 71, 74 },
  { 62, 66, 69 },
  { 67, 71, 74 },
  { 69, 73, 76 },
};

#define ROUND_TICKS   (8 * HALF)
#define CANON_ROUNDS  3
#define CANON_TICKS   (CANON_ROUNDS * ROUND_TICKS)
#define CANON_PARTS   3

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

static int buildCanonRaw(ScoreNote *notes, int maxNotes) {
  int c = 0;

  for (int r = 0; r < CANON_ROUNDS; r++) {
    const uint16_t base = (uint16_t)(r * ROUND_TICKS);

    for (int i = 0; i < 8; i++)
      put(notes, c, maxNotes, (uint16_t)(base + i * HALF), HALF, kBass[i], 88, 2);

    if (r == 0) {

      for (int i = 0; i < 8; i++) {
        put(notes, c, maxNotes, (uint16_t)(base + i * HALF), HALF, kMelA[i],  105, 0);
        put(notes, c, maxNotes, (uint16_t)(base + i * HALF), HALF, kInner[i],  78, 1);
      }

    } else if (r == 1) {

      for (int i = 0; i < 8; i++) {
        const uint16_t t = (uint16_t)(base + i * HALF);
        put(notes, c, maxNotes, t,           QUARTER, kMelA[i], 105, 0);

        put(notes, c, maxNotes, (uint16_t)(t + QUARTER), QUARTER,
            kMelA[(i + 1) % 8], 98, 0);

        put(notes, c, maxNotes, t, HALF, kInner[i], 74, 1);
      }

    } else {

      for (int i = 0; i < 8; i++) {
        const uint16_t t = (uint16_t)(base + i * HALF);
        const uint8_t *ch = kChord[i];

        put(notes, c, maxNotes, (uint16_t)(t + 0 * EIGHTH), EIGHTH, ch[0], 100, 0);
        put(notes, c, maxNotes, (uint16_t)(t + 1 * EIGHTH), EIGHTH, ch[1],  96, 0);
        put(notes, c, maxNotes, (uint16_t)(t + 2 * EIGHTH), EIGHTH, ch[2], 100, 0);
        put(notes, c, maxNotes, (uint16_t)(t + 3 * EIGHTH), EIGHTH, ch[1],  96, 0);

        put(notes, c, maxNotes, t, QUARTER, kInner[i], 72, 1);
        put(notes, c, maxNotes, (uint16_t)(t + QUARTER), QUARTER,
            kInner[(i + 1) % 8], 70, 1);
      }
    }
  }
  return c;
}

static int pickOctaveShift(int pmin, int pmax) {
  int best = 0, bestCost = 1 << 30;
  for (int k = -36; k <= 36; k += 12) {
    const int lo = pmin + k, hi = pmax + k;
    if (lo < 0 || hi > 127) continue;
    const int below = (gScaleLo > lo) ? (gScaleLo - lo) : 0;
    const int above = (hi > gScaleHi) ? (hi - gScaleHi) : 0;
    const int cost  = below * 2 + above;

    if (cost < bestCost || (cost == bestCost && abs(k) < abs(best))) {
      bestCost = cost;
      best = k;
    }
  }
  return best;
}

static int mergeUnisons(ScoreNote *n, int cnt) {
  for (int i = 0; i < cnt; i++) {
    for (int j = i + 1; j < cnt; j++) {
      if (n[i].midi != n[j].midi) continue;
      const int ai = n[i].tick, bi = ai + n[i].dur;
      const int aj = n[j].tick, bj = aj + n[j].dur;
      if (aj >= bi || ai >= bj) continue;

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
      j--;
    }
  }
  return cnt;
}

static int buildCanon(ScoreNote *notes, int maxNotes) {
  const int cnt = buildCanonRaw(notes, maxNotes);
  if (cnt <= 0) return cnt;

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

  for (int i = 0; i < cnt; i++) {
    const int p = notes[i].part;
    if (p < 0 || p >= CANON_PARTS) continue;
    int m = (int)notes[i].midi + shift[p];

    while (m < 0)   m += 12;
    while (m > 127) m -= 12;
    notes[i].midi = (uint8_t)m;
  }
  for (int p = 0; p < CANON_PARTS; p++) gCanonShift[p] = shift[p];

  return mergeUnisons(notes, cnt);
}

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

    return (uint16_t)(CANON_TICKS + HALF);
  }

  return (uint16_t)(scaleNoteCount() * SCALE_TICKS + SCALE_TICKS);
}

const char *scoreName() {

  static const char *kNames[12] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
  static char buf[24];

  if (gMode == TC_SCORE_CANON) {

    snprintf(buf, sizeof(buf), "Canon in D %+d", gCanonShift[0]);
    return buf;
  }

  snprintf(buf, sizeof(buf), "%s%d-%s%d scale",
           kNames[((gScaleLo % 12) + 12) % 12], gScaleLo / 12 - 1,
           kNames[((gScaleHi % 12) + 12) % 12], gScaleHi / 12 - 1);
  return buf;
}

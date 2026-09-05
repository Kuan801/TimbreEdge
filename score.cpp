#include "score.h"
#include <stdlib.h>   // abs

// ============================================================================
//  The two scores
//
//  Chromatic  not there to be listened to, there to be measured: every note is
//             separate and non-overlapping, so tools/evaluate.py can cut them
//             apart automatically and compute envelope correlation and spectral
//             distance against the matching source material.
//  Canon      not there to be measured, there to be listened to: three voices
//             sounding at once, to judge whether this timbre sounds right when it
//             plays actual music. Note-by-note comparison means nothing for it.
//
//  The canon was once removed wholesale (precisely because "it cannot be compared
//  note by note"); the old code is in 舊版備份/score_canon.cpp. The version brought
//  back does one thing more: pitches are no longer hard-coded, each voice is
//  octave-placed according to the range the timbre bank actually covers -- see
//  pickOctaveShift() below.
// ============================================================================

#define TICKS_PER_BAR   16      // TC_TICKS_PER_BEAT = 4, 4/4 time
#define HALF            8
#define QUARTER         4
#define EIGHTH          2

// ---------------------------------------------------------------------------
//  Which score is to be generated right now
//
//  The default is always the chromatic scale. evaluate.py / bench.py / score_test
//  are all built on it, and "something else is the default after boot" would
//  silently invalidate the existing evaluation baselines without any error.
// ---------------------------------------------------------------------------
static ScoreMode gMode = TC_SCORE_SCALE;

void      scoreSetMode(ScoreMode m) { gMode = m; }
ScoreMode scoreGetMode()            { return gMode; }

// ---------------------------------------------------------------------------
//  Chromatic scale
// ---------------------------------------------------------------------------
#define SCALE_TICKS     HALF

// Defaults to C3~B4 (48~71), same as before the rework, so the evaluation scripts and score_test are unaffected
static int gScaleLo = 48;
static int gScaleHi = 71;

void scoreSetScaleRange(int midiLo, int midiHi) {
  if (midiLo < 0)   midiLo = 0;
  if (midiHi > 127) midiHi = 127;
  if (midiHi < midiLo) { const int t = midiLo; midiLo = midiHi; midiHi = t; }

  // Cap: the whole piece has to fit in the Player's note array. The chromatic
  // scale is one voice, one note per semitone, so just clamp the note count.
  // TC_MAX_NOTES is 224 and is never reached in practice, but the timbre bank
  // could in principle cover a very wide range, so guard it anyway.
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
//  Canon (Canon in D, reduced to three voices)
//
//  Harmonic progression (one half note per chord, 8 chords = 4 bars, three rounds):
//        D    A    Bm   F#m  G    D    G    A
//
//  The three rounds are deliberately three different textures: section A lays the
//  harmony out plainly, section B breaks the theme into quarter notes, section C
//  is eighth-note arpeggios. How one timbre differs between "long notes" and
//  "fast runs" is what this score is really meant to reveal -- the quality of
//  attack and release only becomes audible in section C.
// ---------------------------------------------------------------------------
static const uint8_t kBass[8]  = { 50, 57, 59, 54, 55, 50, 55, 57 };
//                                 D3  A3  B3  F#3 G3  D3  G3  A3

// Section A theme (half notes): F#5 E5 D5 C#5 B4 A4 B4 C#5
static const uint8_t kMelA[8]  = { 78, 76, 74, 73, 71, 69, 71, 73 };

// Inner voice: thirds/sixths below the theme
static const uint8_t kInner[8] = { 74, 73, 71, 69, 67, 66, 67, 69 };
//                                 D5  C#5 B4  A4  G4  F#4 G4  A4

// The triad of each chord, used by the section C arpeggios
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

#define ROUND_TICKS   (8 * HALF)      // One round = 8 half notes = 64 ticks
#define CANON_ROUNDS  3
#define CANON_TICKS   (CANON_ROUNDS * ROUND_TICKS)
#define CANON_PARTS   3

// How many semitones each voice was actually shifted last time (a multiple of 12), for the serial printout
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

// Original key (D major, bass down to D3, melody up to F#5). Transposition is done
// in one place afterwards, so the generator stays written "as scored" and changing
// the melody does not mean thinking about transposition at the same time.
static int buildCanonRaw(ScoreNote *notes, int maxNotes) {
  int c = 0;

  for (int r = 0; r < CANON_ROUNDS; r++) {
    const uint16_t base = (uint16_t)(r * ROUND_TICKS);

    // ---- Bass: the same in all three rounds, half notes --------------
    for (int i = 0; i < 8; i++)
      put(notes, c, maxNotes, (uint16_t)(base + i * HALF), HALF, kBass[i], 88, 2);

    if (r == 0) {
      // ---- A: theme and inner voice in half notes, harmony first ----
      for (int i = 0; i < 8; i++) {
        put(notes, c, maxNotes, (uint16_t)(base + i * HALF), HALF, kMelA[i],  105, 0);
        put(notes, c, maxNotes, (uint16_t)(base + i * HALF), HALF, kInner[i],  78, 1);
      }

    } else if (r == 1) {
      // ---- B: theme split into quarter notes, plus a descending line -
      for (int i = 0; i < 8; i++) {
        const uint16_t t = (uint16_t)(base + i * HALF);
        put(notes, c, maxNotes, t,           QUARTER, kMelA[i], 105, 0);
        // Second half of the beat steps to the next chord's note, so the line flows instead of marking time
        put(notes, c, maxNotes, (uint16_t)(t + QUARTER), QUARTER,
            kMelA[(i + 1) % 8], 98, 0);
        // Inner voice stays in half notes as a pad
        put(notes, c, maxNotes, t, HALF, kInner[i], 74, 1);
      }

    } else {
      // ---- C: eighth-note arpeggios, the canon's famous line ---------
      for (int i = 0; i < 8; i++) {
        const uint16_t t = (uint16_t)(base + i * HALF);
        const uint8_t *ch = kChord[i];
        // root -> third -> fifth -> third, four eighth notes exactly fill one half note
        put(notes, c, maxNotes, (uint16_t)(t + 0 * EIGHTH), EIGHTH, ch[0], 100, 0);
        put(notes, c, maxNotes, (uint16_t)(t + 1 * EIGHTH), EIGHTH, ch[1],  96, 0);
        put(notes, c, maxNotes, (uint16_t)(t + 2 * EIGHTH), EIGHTH, ch[2], 100, 0);
        put(notes, c, maxNotes, (uint16_t)(t + 3 * EIGHTH), EIGHTH, ch[1],  96, 0);
        // Inner voice in quarter notes, so it does not fight the arpeggio
        put(notes, c, maxNotes, t, QUARTER, kInner[i], 72, 1);
        put(notes, c, maxNotes, (uint16_t)(t + QUARTER), QUARTER,
            kInner[(i + 1) % 8], 70, 1);
      }
    }
  }
  return c;
}

// ---------------------------------------------------------------------------
//  Octave placement: put the canon inside the range this machine has actually
//  been verified on
//
//  Why it is needed: the score is in D major with the bass down to D3(50). The
//  timbre bank often covers only C4~B4, so playing it in the original key means
//  transposing every bass note down more than 10 semitones -- it stops sounding
//  like the instrument, and it is easily mistaken for a broken synthesizer (the
//  README's "the chromatic scale used to be hard-coded to C3~B4" is the same pit;
//  the conclusion that time was that the range has to follow the timbre bank).
//
//  The target range is the same [gScaleLo, gScaleHi] the chromatic scale uses,
//  i.e. "the range the bank covers, plus one octave above it".
//
//  Three design decisions:
//
//  1) Only whole octaves are moved. Shifting by a multiple of 12 does not change
//     the key, so the canon is still in D major; shifting by anything else turns
//     it into a different piece, which is not this code's job.
//
//  2) Each voice picks its own octave, rather than moving the whole piece. The
//     original spans 29 semitones (D3~F#5) while a C4~B4 bank leaves a usable
//     window of only 24 -- moving the piece as a whole always puts one end out of
//     bounds. Per-voice placement lets all three lines fit, and leaves the
//     intervals within each line completely unchanged (none of that "a few notes
//     suddenly jump an octave" folding that wrecks the melody). Close position is
//     a common simplification anyway, so it does not sound odd musically.
//
//  3) In the cost function, going out of bounds downwards weighs twice as much as
//     upwards. Transposing down has to conjure a fundamental lower than anything
//     in the material, while transposing up is only a rearrangement of harmonics
//     that are already there -- the README's discussion of transposition has the
//     measurements; the two are not equivalent.
// ---------------------------------------------------------------------------
static int pickOctaveShift(int pmin, int pmax) {
  int best = 0, bestCost = 1 << 30;
  for (int k = -36; k <= 36; k += 12) {
    const int lo = pmin + k, hi = pmax + k;
    if (lo < 0 || hi > 127) continue;
    const int below = (gScaleLo > lo) ? (gScaleLo - lo) : 0;
    const int above = (hi > gScaleHi) ? (hi - gScaleHi) : 0;
    const int cost  = below * 2 + above;
    // On a tie, take the smallest shift: no reason to move an extra octave
    if (cost < bestCost || (cost == bestCost && abs(k) < abs(best))) {
      bestCost = cost;
      best = k;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
//  No two identical pitches at the same instant
//
//  This is a Player limitation, not a musical nicety: note-off is keyed on the
//  *pitch* (what _pend holds is the midi number), so when two notes of the same
//  pitch overlap, whichever ends first also shuts off the one still sounding.
//  What you hear is "the bass inexplicably cuts out for half a beat", and only in
//  a few bars -- practically impossible to locate by ear.
//
//  Why the canon runs into it: with a bank covering only one octave, the bass is
//  placed an octave up and lands in the same range as the inner voice, and 4 of
//  the 8 chords then produce a unison. The version with hard-coded pitches never
//  hit it (the bass sat one octave below by construction); this is a new
//  situation brought in by "follow the timbre bank".
//
//  The fix is to merge them into one note rather than throw one away: union of
//  the times, the larger velocity, and the voice of "whichever note is longer"
//  (the longer one is usually the pad, so the pan does not jump left and right).
//  Only one note was ever audible anyway; merging just removes one pointless
//  overlap.
// ---------------------------------------------------------------------------
static int mergeUnisons(ScoreNote *n, int cnt) {
  for (int i = 0; i < cnt; i++) {
    for (int j = i + 1; j < cnt; j++) {
      if (n[i].midi != n[j].midi) continue;
      const int ai = n[i].tick, bi = ai + n[i].dur;
      const int aj = n[j].tick, bj = aj + n[j].dur;
      if (aj >= bi || ai >= bj) continue;              // No overlap, the two notes are both fine as they are

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
      j--;                                             // A different note is at position j now, check it again
    }
  }
  return cnt;
}

static int buildCanon(ScoreNote *notes, int maxNotes) {
  const int cnt = buildCanonRaw(notes, maxNotes);
  if (cnt <= 0) return cnt;

  // ---- Each voice's original-key range and mean pitch --------------------
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

  // ---- Voices must not cross over -----------------------------------------
  //
  // Per-voice octave choice has a side effect: when the bank sits very low, the
  // inner voice and the melody get pushed all the way down, while the bass is
  // already at the bottom and does not move -- so the bass ends up above the
  // melody and it no longer sounds like a canon. "bass <= inner voice <= melody"
  // is a hard constraint here, judged on mean pitch (the three lines overlap by
  // nature, so judging on the highest/lowest note would be far too twitchy).
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

  // ---- Apply --------------------------------------------------------------
  for (int i = 0; i < cnt; i++) {
    const int p = notes[i].part;
    if (p < 0 || p >= CANON_PARTS) continue;
    int m = (int)notes[i].midi + shift[p];
    // Safety net: never emit an out-of-range MIDI pitch under any circumstances
    // (fold it back by octaves, do not clamp -- clamping squashes several notes
    // onto the same pitch, which sounds like the synth is stuck)
    while (m < 0)   m += 12;
    while (m > 127) m -= 12;
    notes[i].midi = (uint8_t)m;
  }
  for (int p = 0; p < CANON_PARTS; p++) gCanonShift[p] = shift[p];

  // Unisons only show up once the transposition is done, so the merge goes last
  return mergeUnisons(notes, cnt);
}

// ---------------------------------------------------------------------------
//  The Player only ever scans forward: fireTick() advances only when
//  _notes[_cursor].tick == _tick. So the notes must be sorted by ascending tick,
//  otherwise once the cursor passes a note it can never get back and everything
//  after it is silent.
//
//  The generator above is written by voice (the bass for a whole round, then the
//  melody for a whole round), which reads well but is in the wrong order -- the
//  first run gave exactly that: only the section A bass sounding, then 30 seconds
//  of silence. Sorting here in one place lets the generator stay written by voice.
//
//  Insertion sort: one pass over 112 notes takes under 0.1 ms, and the array is
//  already nearly sorted (each voice is ascending within itself), so the actual
//  comparison count is far below the worst case.
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
    // Leave one half note at the end: the release of section C's last arpeggio is
    // still sounding, and stopping early chops the tail off -- recorded to a WAV,
    // that cut is particularly obvious
    return (uint16_t)(CANON_TICKS + HALF);
  }
  // One extra SCALE_TICKS at the end as a release tail, so the last note is not chopped off
  return (uint16_t)(scaleNoteCount() * SCALE_TICKS + SCALE_TICKS);
}

const char *scoreName() {
  // The range is dynamic, so the name has to follow it, or the panel shows a range that is not the real one
  static const char *kNames[12] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
  static char buf[24];

  if (gMode == TC_SCORE_CANON) {
    // Only octaves move so the key is unchanged, but which octave the melody landed in matters to the ear, so put it in the name
    snprintf(buf, sizeof(buf), "Canon in D %+d", gCanonShift[0]);
    return buf;
  }

  snprintf(buf, sizeof(buf), "%s%d-%s%d scale",
           kNames[((gScaleLo % 12) + 12) % 12], gScaleLo / 12 - 1,
           kNames[((gScaleHi % 12) + 12) % 12], gScaleHi / 12 - 1);
  return buf;
}

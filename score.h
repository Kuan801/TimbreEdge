// ============================================================================
//  score.h  -  scores: chromatic scale (for measurement) + canon (for listening)
//
//  The two scores serve two purposes and do not replace each other:
//
//    Chromatic  every note separate and non-overlapping, so tools/evaluate.py can
//               cut them apart and compare note by note. Every published number
//               comes from this score, and it is always the default mode.
//    Canon      three voices sounding together, to hear whether the timbre holds
//               up when it actually plays music. Note-by-note comparison is
//               meaningless here; its purpose is the ear.
//
//  The canon was removed at one point (old code in 舊版備份/score_canon.cpp) and
//  has been brought back, the difference being that pitches are no longer
//  hard-coded: each voice is octave-placed according to the range the timbre bank
//  actually covers. See the notes in score.cpp.
//
//  Tick resolution = sixteenth note (TC_TICKS_PER_BEAT = 4)
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"

struct ScoreNote {
  uint16_t tick;      // Start tick
  uint16_t dur;       // Length in ticks
  uint8_t  midi;
  uint8_t  vel;       // 0..127
  uint8_t  part;      // 0=melody 1=inner voice 2=bass
};

// ---------------------------------------------------------------------------
//  Which score to generate
//
//  The default is always the chromatic scale: evaluate.py, bench.py and
//  score_test are all built on it, and any change that makes something else the
//  default after boot would silently invalidate every existing baseline.
//  The canon is only selected on an explicit request (serial j / menu Canon) and
//  does not stay selected afterwards -- switching back is the caller's job.
// ---------------------------------------------------------------------------
enum ScoreMode : uint8_t {
  TC_SCORE_SCALE = 0,       // Chromatic scale
  TC_SCORE_CANON = 1,       // Canon
};

void      scoreSetMode(ScoreMode m);
ScoreMode scoreGetMode();

// Generate the whole score (returns the note count); notes needs at least TC_MAX_NOTES slots
int         buildScore(ScoreNote *notes, int maxNotes);
uint16_t    scoreTotalTicks();
const char *scoreName();

// How many semitones each voice was actually shifted the last time the canon was
// generated (always a multiple of 12). part: 0 melody, 1 inner voice, 2 bass.
// This is printed to the serial port -- anything the panel cannot show has to be
// findable in the log, otherwise "it sounds an octave high" is impossible to trace.
int         scoreCanonShift(int part);

// ---------------------------------------------------------------------------
//  Range of the chromatic scale
//
//  This used to be hard-coded to C3~B4, a range picked out of thin air: with a
//  bank covering only C4~B4, C3 has to be transposed down 12 semitones, so of
//  course it does not sound right -- and that gets mistaken for a synthesizer bug.
//
//  It is now supplied by the caller, in practice "the bank's range plus one
//  octave": the lower half is interpolation (covered by the bank), the upper half
//  is a real test of transposition and formant correction, and one performance
//  makes the difference between them audible.
//
//  Left unset it stays C3~B4, so existing evaluation flows and score_test are
//  unaffected.
// ---------------------------------------------------------------------------
void scoreSetScaleRange(int midiLo, int midiHi);
void scoreGetScaleRange(int *midiLo, int *midiHi);

// Total length of the piece (ticks)


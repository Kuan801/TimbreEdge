#pragma once

#include <Arduino.h>
#include "config.h"

struct ScoreNote {
  uint16_t tick;
  uint16_t dur;
  uint8_t  midi;
  uint8_t  vel;
  uint8_t  part;
};

enum ScoreMode : uint8_t {
  TC_SCORE_SCALE = 0,
  TC_SCORE_CANON = 1,
};

void      scoreSetMode(ScoreMode m);
ScoreMode scoreGetMode();

int         buildScore(ScoreNote *notes, int maxNotes);
uint16_t    scoreTotalTicks();
const char *scoreName();

int         scoreCanonShift(int part);

void scoreSetScaleRange(int midiLo, int midiHi);
void scoreGetScaleRange(int *midiLo, int *midiHi);

#pragma once

#include <stddef.h>
#include <stdint.h>

enum RecVerdict : uint8_t {
  REC_OK   = 0,
  REC_WARN = 1,
  REC_BAD  = 2,
};

struct RecCheck {
  bool  analysisOk;
  float peak;
  float clipRatio;
  float noiseFloor;
  int   onsets;
  float noteDur;
  float f0;
  float decayPerSec;
};

RecVerdict recCheckEval(const RecCheck &in,
                        char *reason, size_t reasonCap,
                        char *fix,    size_t fixCap);

const char *recVerdictTitle(RecVerdict v);

#define RECCHK_SNR_UNKNOWN  (-999.0f)
float recCheckSnrDb(float noiseFloor);
bool  recCheckSnrKnown(float noiseFloor);

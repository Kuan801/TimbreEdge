#include "rec_check.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CLIP_BAD        0.0005f
#define PEAK_TOO_LOW    0.05f
#define PEAK_LOW        0.15f
#define PEAK_HOT        0.90f
#define NOISE_BAD       0.10f
#define NOISE_WARN      0.032f
#define DUR_TOO_SHORT   0.35f
#define DUR_SHORT       0.90f

static void put(char *dst, size_t cap, const char *fmt, ...) {
  if (!dst || !cap) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dst, cap, fmt, ap);
  va_end(ap);
}

bool recCheckSnrKnown(float noiseFloor) { return noiseFloor > 1e-6f; }

float recCheckSnrDb(float noiseFloor) {
  if (!recCheckSnrKnown(noiseFloor)) return RECCHK_SNR_UNKNOWN;
  return -20.0f * log10f(noiseFloor);
}

const char *recVerdictTitle(RecVerdict v) {
  switch (v) {
    case REC_OK:   return "REC OK";
    case REC_WARN: return "REC - CHECK THIS";
    default:       return "REC FAILED";
  }
}

RecVerdict recCheckEval(const RecCheck &in,
                        char *reason, size_t reasonCap,
                        char *fix,    size_t fixCap) {
  if (reason && reasonCap) reason[0] = 0;
  if (fix    && fixCap)    fix[0]    = 0;

  if (!in.analysisOk) {
    put(reason, reasonCap, "analysis failed");
    put(fix,    fixCap,    "play louder / longer");
    return REC_BAD;
  }

  if (in.onsets >= 2) {
    put(reason, reasonCap, "%d notes in one take", in.onsets);
    put(fix,    fixCap,    "play ONCE and hold");
    return REC_BAD;
  }

  if (in.clipRatio > CLIP_BAD) {
    put(reason, reasonCap, "clipped %.1f%%", in.clipRatio * 100.0f);
    put(fix,    fixCap,    "lower mic gain (g)");
    return REC_BAD;
  }

  if (in.peak < PEAK_TOO_LOW &&
      recCheckSnrKnown(in.noiseFloor) && recCheckSnrDb(in.noiseFloor) < 26.0f) {
    put(reason, reasonCap, "too quiet: peak %.2f", in.peak);
    put(fix,    fixCap,    "raise gain or move in");
    return REC_BAD;
  }

  if (recCheckSnrKnown(in.noiseFloor) && in.noiseFloor > NOISE_BAD) {
    put(reason, reasonCap, "noisy: SNR %.0f dB", recCheckSnrDb(in.noiseFloor));
    put(fix,    fixCap,    "quieter room");
    return REC_BAD;
  }

  if (in.noteDur < DUR_TOO_SHORT) {
    put(reason, reasonCap, "note too short: %.2fs", in.noteDur);
    put(fix,    fixCap,    "hold the note 1-2 s");
    return REC_BAD;
  }

  if (in.peak > PEAK_HOT) {
    put(reason, reasonCap, "hot: peak %.2f", in.peak);
    put(fix,    fixCap,    "lower gain a bit (g)");
    return REC_WARN;
  }

  if (in.peak < PEAK_LOW) {
    put(reason, reasonCap, "quiet: peak %.2f", in.peak);
    put(fix,    fixCap,    "target peak 0.3 - 0.8");
    return REC_WARN;
  }

  if (recCheckSnrKnown(in.noiseFloor) && in.noiseFloor > NOISE_WARN) {
    put(reason, reasonCap, "SNR %.0f dB", recCheckSnrDb(in.noiseFloor));
    put(fix,    fixCap,    "noise floor audible");
    return REC_WARN;
  }

  if (in.noteDur < DUR_SHORT) {
    put(reason, reasonCap, "short: %.2fs", in.noteDur);
    put(fix,    fixCap,    "envelope may be cut");
    return REC_WARN;
  }

  if (in.decayPerSec >= 0.97f) {
    put(reason, reasonCap, "no decay measured");
    put(fix,    fixCap,    "ok for organ or bowed");
    return REC_WARN;
  }

  put(reason, reasonCap, "levels look good");
  put(fix,    fixCap,    "added to bank");
  return REC_OK;
}

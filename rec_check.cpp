#include "rec_check.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
//  How these thresholds were chosen
//
//  Every number in this section comes from real bad takes, not from intuition.
//  The material is two REC.WAV files of "a piano C4 played through a phone
//  speaker into the microphone", plus the clean Iowa MIS originals.
//
//    First REC.WAV    peak 1.000, 203 clipped samples (0.0023 of the file)
//                     dynamic range 2.9 dB (the original source is 24.5 dB)
//                     -> the waveform is flattened and almost every harmonic
//                        measured is an artifact of the clipping
//    Second REC.WAV   peak 0.089, no clipping, but the trigger fired 1.7 s early
//                     it triggered on 30~190 Hz room rumble, SNR only 7 dB
//                     -> of a 2 s recording, only the last 0.3 s is the actual note
//
//  One has too high a peak and the other too low, so no single metric catches
//  both kinds of failure. The verdict therefore looks at clipping, level, SNR and
//  onset count together.
//
//  An honest limitation: these thresholds catch failures on the scale of "the
//  recording chain is broken", not "usable but not good enough". This is a health
//  check, not a grade.
// ============================================================================

#define CLIP_BAD        0.0005f   // Clipped fraction. At 0.05% there are already several flat-topped stretches
#define PEAK_TOO_LOW    0.05f     // Below this, quantization noise in the harmonics starts to get out of hand
#define PEAK_LOW        0.15f     // Still analyzable, but the SNR is no longer good
#define PEAK_HOT        0.90f     // Not clipped yet, but one louder take and it will be
#define NOISE_BAD       0.10f     // SNR 20 dB. The second REC.WAV was 0.45 (7 dB)
#define NOISE_WARN      0.032f    // SNR 30 dB
#define DUR_TOO_SHORT   0.35f     // Shorter than this and the envelope cannot show a decay trend
#define DUR_SHORT       0.90f     // Measured in config.h: from 1.0 s, envelope correlation drops to 0.891

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

  // ---- unusable -----------------------------------------------------------
  //
  // Ordered by how quickly the user can fix it, not by severity. When two rules
  // fire at once, report the easiest fix first -- giving them one thing to do is
  // more useful than listing three problems.
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

  // A low peak is not a problem in itself; a low peak buried in noise is.
  //
  // This rule originally looked only at the peak, and duly declared the clean
  // Iowa MIS piano originals unusable -- those files peak at just 0.033 but have
  // 40 dB SNR and are the reference material. A studio master is allowed to sit
  // at a low absolute level; that is a mastering decision, not a quality problem.
  // What actually ruins the analysis is quantization noise creeping into the
  // harmonics, and SNR sees that while peak does not.
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

  // ---- usable, but could be better ----------------------------------------
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

  // A decay >= 0.97 means "this note barely decays at all". Such instruments do
  // exist (organ, sustained strings), so this is not an error -- but on a plucked
  // instrument it usually means the tail was cut off or a steady noise was
  // recorded. Warn only, never fail, because the code has no idea what the user
  // is holding.
  if (in.decayPerSec >= 0.97f) {
    put(reason, reasonCap, "no decay measured");
    put(fix,    fixCap,    "ok for organ or bowed");
    return REC_WARN;
  }

  put(reason, reasonCap, "levels look good");
  put(fix,    fixCap,    "added to bank");
  return REC_OK;
}

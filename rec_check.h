// ============================================================================
//  rec_check.h  -  the verdict on "is this single-note recording usable at all"
//
//  Why it gets its own file: the verdict itself is purely numeric logic, with
//  nothing whatsoever to do with SD, OLED or Arduino. Split out, every rule can
//  be run through on the desktop (tools/sim/reccheck) instead of flashing once
//  per threshold change.
//
//  Design principle: say only what is wrong and how to fix it, never the theory.
//  Someone standing at the machine with an instrument still in their hands needs
//  to know which key to press next, not a diagnostic report.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

enum RecVerdict : uint8_t {
  REC_OK   = 0,   // usable
  REC_WARN = 1,   // usable, but with something obviously worth improving
  REC_BAD  = 2,   // don't use it, record again
};

struct RecCheck {
  bool  analysisOk;     // whether analyzeWavFile() succeeded
  float peak;           // absolute peak over the whole file, 0..1
  float clipRatio;      // fraction of clipped samples
  float noiseFloor;     // mean RMS before the attack (relative to peak)
  int   onsets;         // attack count; only 1 is a single note
  float noteDur;        // usable note length (seconds)
  float f0;             // fundamental frequency, Hz
  float decayPerSec;    // sustain decay factor per second
};

// reason / fix are one line each, sized to the OLED's 21 characters (anything
// longer gets truncated). Both may be passed as nullptr.
RecVerdict recCheckEval(const RecCheck &in,
                        char *reason, size_t reasonCap,
                        char *fix,    size_t fixCap);

// short heading for the panel: "REC OK" / "REC - CHECK THIS" / "REC FAILED"
const char *recVerdictTitle(RecVerdict v);

// Signal-to-noise ratio (dB). noiseFloor is a linear ratio relative to the peak.
//
// Not being able to measure it really does happen: continuous sampling mode has a
// 93 ms pre-buffer, and the moment a note triggers the file is written from the
// very start of that buffer, so the attack lands exactly in bin 0 with nothing
// ahead of it to serve as a noise floor. In that case it returns
// RECCHK_SNR_UNKNOWN and the verdict has to skip every SNR-related rule —
// "cannot be measured" and "very clean" are two different things, and the former
// is no evidence of good quality.
#define RECCHK_SNR_UNKNOWN  (-999.0f)
float recCheckSnrDb(float noiseFloor);
bool  recCheckSnrKnown(float noiseFloor);

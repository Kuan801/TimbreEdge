// ============================================================================
//  trigger.h  -  continuous sampling: deciding "when should recording start"
//
//  Split into its own file for the same reason as rec_check: not one line in
//  here touches SD, OLED or Arduino -- it is purely a state machine that takes
//  block peaks in and answers whether to trigger. Split out, every path can be
//  driven with synthetic signals on the desktop (tools/sim/trigger_test).
//
//  --- Why it was rewritten -------------------------------------------------
//
//  The old version used the fixed threshold TC_TRIG_LEVEL = 0.035 and kept
//  triggering with nobody playing. Only measuring the files it actually
//  recorded showed how little headroom there is:
//
//      background noise block peak   median 0.021   99% 0.028   max 0.030
//      the quietest note             median block peak 0.053
//      fixed threshold               0.035
//
//  The background maximum sits only 1.16x below the threshold (1.3 dB), and the
//  weakest note only 1.76x above the background maximum (4.9 dB). With margins
//  like that, one more fan in the room records garbage all night.
//
//  So two things changed:
//    1) The threshold is no longer hard-coded but "measured ambient noise x
//       margin" -- a noisy room raises it by itself.
//    2) A stretch of genuine silence has to be seen before triggering. The old
//       version had this guard backwards (see below).
//
//  --- The guard the old version had backwards --------------------------------
//
//      if (pk < _thresh * 0.5f) _quietSince = millis();
//      ...
//      if (++_hotBlocks >= N && (millis() - _quietSince) > TC_REARM_SILENT_MS)
//
//  The intent was "after a take, stay quiet for a while before re-arming", but
//  what it actually says is "the last time silence was seen was more than
//  400 ms ago". As long as the background stays above half the threshold,
//  _quietSince never updates and that difference only grows --
//  that is, the noisier the room, the less this guard blocks, exactly the
//  opposite of the intent.
//
//  It is a latch now: _rearmed is only set after a *continuous*
//  TC_REARM_SILENT_MS of silence, and triggering consumes it. Sustained noise
//  never accumulates that unbroken stretch, so it never arms.
// ============================================================================
#pragma once

#include <stdint.h>
#include "config.h"

// ---------------------------------------------------------------------------
//  The "level" of one audio block: the TC_BLOCK_TOPK-th largest sample magnitude
//
//  It used to be the plain maximum. The problem is that the maximum has no
//  resistance at all to single-sample impulses, and on real hardware there are
//  plenty of those (digital coupling): on one unit the block RMS while quiet is
//  only 0.002, yet 4~8% of the blocks carry a single 0.19~0.28 needle. Judged
//  by the maximum, those blocks look exactly like someone playing.
//
//  The 4th largest separates them, and **the unit does not change**:
//    real tone     dozens of the 128 samples sit near the peak (a 440 Hz block
//                  holds 1.3 periods, 29% of its samples have |sin| > 0.9), so
//                  the 4th largest ≈ the maximum.
//    lone impulse  only 1~3 samples are large, so the 4th largest falls straight
//                  back to the noise floor.
//
//  Keeping the unit matters: TC_TRIG_LEVEL and every threshold number in
//  trigger_test was measured as the "block peak" of real recordings. Change the
//  scale (to block RMS, say) and the whole set needs recalibrating, and every
//  measurement in the README becomes worthless.
//
//  Here rather than in recorder.cpp: this is part of "when should recording
//  start", and it is a pure function -- here it is testable on the desktop
//  (tools/sim/trigger_test).
// ---------------------------------------------------------------------------
#define TC_BLOCK_TOPK 4
float tcBlockLevel(const int16_t *src, int n);

class TriggerGate {
public:
  // baseThreshold: the floor the user set (`s 0.01` or TC_TRIG_LEVEL).
  // The effective threshold never goes below it, but ambient noise can raise it.
  void arm(float baseThreshold, uint32_t nowMs);

  // Feed one audio block's peak (0..1). Returns true meaning "start recording now".
  bool feed(float blockPeak, uint32_t nowMs);

  // Call after finishing a take: clear the armed state and force it to wait for
  // silence again. Without this the tail of the note triggers again at once.
  void noteRecorded(uint32_t nowMs);

  bool  calibrating() const { return _calibrating; }
  float ambient()     const { return _ambient; }     // measured ambient noise peak
  float threshold()   const { return _thresh;  }     // effective threshold
  float level()       const { return _level;   }     // smoothed level, for the UI
  bool  armedReady()  const { return _rearmed; }

  // How many isolated spikes "far above ambient" were seen during calibration.
  // With ambient estimated from a high percentile these no longer wreck the
  // threshold, but they are still a symptom of a hardware problem (digital
  // coupling, grounding) and the user should know -- do not silently tolerate it.
  int   calSpikes()   const { return _calSpikes; }

  // How many dB above the ambient noise the signal was at the last trigger.
  // This is the most direct single number for "is this environment usable at
  // all"; below TC_TRIG_MIN_HEADROOM_DB warn the user instead of quietly
  // recording a pile of junk files.
  float lastHeadroomDb() const;

  // How many times the current threshold exceeds the floor the user set. > 1 means noise dominates.
  float ambientRatio() const { return _base > 0 ? _thresh / _base : 1.0f; }

private:
  // During calibration keep the "few largest block peaks" and finally take the
  // CAL_KEEP-th largest as the ambient noise.
  //
  // It used to be the maximum, on the grounds that "false triggers come from
  // spikes, not from the average", and a measured peak/median of only 1.46 on a
  // clean background meant the maximum was not over-conservative. That reasoning
  // holds on a clean background, but the maximum has a breakdown point of 0 --
  // **a single outlier destroys it**.
  //
  // And it really happened: one unit's input occasionally produced isolated
  // digital spikes (AC RMS only 0.001, the spike up to 0.28). Hit one inside the
  // 600 ms of calibration and ambient is measured as 0.2767, threshold =
  // 0.2767 × 1.5 = 0.415, after which nothing you play can ever trigger, while
  // the screen only says "waiting… level 0.06 / threshold 0.41".
  //
  // 600 ms is about 207 blocks, so the 8th largest ≈ the 96th percentile: it can
  // swallow 7 spikes, and on a clean background it barely differs from the
  // maximum (measured background: 99% 0.028, max 0.030). Same reasoning as the
  // "median of 5 points" used when the analyzer measures noiseGain.
  static const int CAL_KEEP = 8;
  float    _calTop[CAL_KEEP];
  int      _calN       = 0;
  int      _calSpikes  = 0;

  float    _base       = TC_TRIG_LEVEL;
  float    _ambient    = 0.0f;
  float    _thresh     = TC_TRIG_LEVEL;
  float    _level      = 0.0f;
  float    _lastTrigPk = 0.0f;
  float    _lastAmb    = 0.0f;
  bool     _calibrating = false;
  bool     _rearmed     = false;
  bool     _quiet       = false;
  uint32_t _calUntil    = 0;
  uint32_t _quietSince  = 0;
  int      _hot         = 0;

  void  recomputeThreshold();
  float quietLevel() const;
};

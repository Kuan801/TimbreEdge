#include "trigger.h"

#include <math.h>

float tcBlockLevel(const int16_t *src, int n) {
  if (!src || n <= 0) return 0.0f;
  // Keep the top K (descending). K is only 4, so plain insertion sort -- at most 4 compares per sample.
  float top[TC_BLOCK_TOPK];
  for (int i = 0; i < TC_BLOCK_TOPK; i++) top[i] = 0.0f;
  for (int i = 0; i < n; i++) {
    const int16_t v = src[i];
    const float a = (v < 0 ? -(float)v : (float)v) * (1.0f / 32768.0f);
    for (int k = 0; k < TC_BLOCK_TOPK; k++) {
      if (a > top[k]) {
        for (int j = TC_BLOCK_TOPK - 1; j > k; j--) top[j] = top[j - 1];
        top[k] = a;
        break;
      }
    }
  }
  // With fewer than K samples there is nothing to reject, so fall back to the maximum
  return (n >= TC_BLOCK_TOPK) ? top[TC_BLOCK_TOPK - 1] : top[0];
}

void TriggerGate::recomputeThreshold() {
  const float fromAmbient = _ambient * TC_TRIG_MARGIN;
  _thresh = (fromAmbient > _base) ? fromAmbient : _base;
}

// "Quiet" = back down to the ambient noise level, not "some fraction of the threshold".
// The reasoning is under TC_TRIG_QUIET_MARGIN in config.h -- basing it on the threshold lets
// the ambient noise cross the quiet line by itself, so it never manages to arm.
float TriggerGate::quietLevel() const {
  const float fromAmbient = _ambient * TC_TRIG_QUIET_MARGIN;
  const float fromBase    = _base * TC_TRIG_QUIET_FRAC;
  return (fromAmbient > fromBase) ? fromAmbient : fromBase;
}

void TriggerGate::arm(float baseThreshold, uint32_t nowMs) {
  _base        = (baseThreshold > 0.0f) ? baseThreshold : TC_TRIG_LEVEL;
  _ambient     = 0.0f;
  _thresh      = _base;
  _level       = 0.0f;
  _lastTrigPk  = 0.0f;
  _lastAmb     = 0.0f;
  _calibrating = true;
  _calN        = 0;
  _calSpikes   = 0;
  for (int i = 0; i < CAL_KEEP; i++) _calTop[i] = 0.0f;
  _rearmed     = false;
  _quiet       = false;
  _calUntil    = nowMs + TC_TRIG_CAL_MS;
  _quietSince  = nowMs;
  _hot         = 0;
}

void TriggerGate::noteRecorded(uint32_t nowMs) {
  // The note is still sounding the moment recording ends. Clear the armed state and restart
  // the quiet timer, so the tail isn't taken for the next note.
  _rearmed    = false;
  _quiet      = false;
  _hot        = 0;
  _quietSince = nowMs;
}

bool TriggerGate::feed(float blockPeak, uint32_t nowMs) {
  _level = _level * 0.7f + blockPeak * 0.3f;

  // ---- Listen for a moment first, to measure how noisy this room is ------
  //
  // A high percentile rather than the mean: what causes false triggers is peaks, not averages.
  // But not the maximum either -- one isolated digital spike would put the ambient estimate
  // through the roof and it would never trigger again. Keep the largest CAL_KEEP values and take
  // the smallest of those (≈96th percentile). Full reasoning in the CAL_KEEP notes in trigger.h.
  if (_calibrating) {
    // Insertion sort, kept descending. CAL_KEEP is only 8, at most 8 compares per block.
    for (int i = 0; i < CAL_KEEP; i++) {
      if (blockPeak > _calTop[i]) {
        for (int j = CAL_KEEP - 1; j > i; j--) _calTop[j] = _calTop[j - 1];
        _calTop[i] = blockPeak;
        break;
      }
    }
    if (_calN < 1000000) _calN++;
    if ((int32_t)(nowMs - _calUntil) >= 0) {
      _calibrating = false;
      // With fewer blocks than CAL_KEEP there aren't enough samples to reject outliers (600 ms
      // normally gives about 200; only tests get here), so fall back to "the smallest one we have".
      const int idx = (_calN >= CAL_KEEP) ? (CAL_KEEP - 1)
                                          : (_calN > 0 ? _calN - 1 : 0);
      _ambient = _calTop[idx];
      // Of the values that got rejected, how many were real peaks "far above ambient".
      // The threshold no longer depends on them, but they are a hardware symptom in themselves, so report them.
      _calSpikes = 0;
      for (int i = 0; i < idx; i++)
        if (_calTop[i] > _ambient * 4.0f) _calSpikes++;
      recomputeThreshold();
      // Those 600 ms were already an observed stretch of quiet, so the extra 400 ms wait can be skipped.
      //
      // The test is "ambient below the floor the user set". It cannot compare against _thresh --
      // _thresh is _ambient × 1.5 by construction, so that comparison always holds and decides nothing.
      // (A desktop test caught this one: feed a full-scale signal during calibration and it still said armed.)
      //
      // Ambient noisier than the user's floor means this room's noise is already at signal level.
      // Then take the normal path: wait for a genuine stretch of quiet, and let the user see the threshold go up.
      _rearmed    = (_ambient < _base);
      _quiet      = _rearmed;
      _quietSince = nowMs;
    }
    return false;
  }

  // ---- Quiet tracking (the only condition for arming) ---------------------
  if (blockPeak < quietLevel()) {
    if (!_quiet) { _quiet = true; _quietSince = nowMs; }
    if ((int32_t)(nowMs - _quietSince) >= (int32_t)TC_REARM_SILENT_MS) {
      _rearmed = true;
      // Only let the ambient estimate follow along while it is quiet. Very slow coefficient (0.5%
      // per block, one block per 2.9 ms -> time constant about 0.6 s), so the threshold rises when a
      // fan starts up, but one note's attack can't lift it -- and this branch only runs below the threshold anyway.
      _ambient = _ambient * 0.995f + blockPeak * 0.005f;
      recomputeThreshold();
    }
  } else {
    _quiet = false;
  }

  // ---- Trigger ------------------------------------------------------------
  if (blockPeak >= _thresh) {
    _hot++;
    if (_hot >= TC_TRIG_BLOCKS && _rearmed) {
      _lastTrigPk = blockPeak;
      _lastAmb    = _ambient;
      _rearmed    = false;
      _hot        = 0;
      return true;
    }
  } else {
    _hot = 0;
  }
  return false;
}

float TriggerGate::lastHeadroomDb() const {
  if (_lastTrigPk <= 0.0f) return 0.0f;
  // Ambient can legitimately measure 0 (a perfectly silent room + no microphone connected).
  // Headroom is infinite in that case; return a large but finite number, not inf.
  if (_lastAmb <= 1e-5f) return 99.0f;
  return 20.0f * log10f(_lastTrigPk / _lastAmb);
}

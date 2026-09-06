#include "trigger.h"

#include <math.h>

float tcBlockLevel(const int16_t *src, int n) {
  if (!src || n <= 0) return 0.0f;

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

  return (n >= TC_BLOCK_TOPK) ? top[TC_BLOCK_TOPK - 1] : top[0];
}

void TriggerGate::recomputeThreshold() {
  const float fromAmbient = _ambient * TC_TRIG_MARGIN;
  _thresh = (fromAmbient > _base) ? fromAmbient : _base;
}

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

  _rearmed    = false;
  _quiet      = false;
  _hot        = 0;
  _quietSince = nowMs;
}

bool TriggerGate::feed(float blockPeak, uint32_t nowMs) {
  _level = _level * 0.7f + blockPeak * 0.3f;

  if (_calibrating) {

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

      const int idx = (_calN >= CAL_KEEP) ? (CAL_KEEP - 1)
                                          : (_calN > 0 ? _calN - 1 : 0);
      _ambient = _calTop[idx];

      _calSpikes = 0;
      for (int i = 0; i < idx; i++)
        if (_calTop[i] > _ambient * 4.0f) _calSpikes++;
      recomputeThreshold();

      _rearmed    = (_ambient < _base);
      _quiet      = _rearmed;
      _quietSince = nowMs;
    }
    return false;
  }

  if (blockPeak < quietLevel()) {
    if (!_quiet) { _quiet = true; _quietSince = nowMs; }
    if ((int32_t)(nowMs - _quietSince) >= (int32_t)TC_REARM_SILENT_MS) {
      _rearmed = true;

      _ambient = _ambient * 0.995f + blockPeak * 0.005f;
      recomputeThreshold();
    }
  } else {
    _quiet = false;
  }

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

  if (_lastAmb <= 1e-5f) return 99.0f;
  return 20.0f * log10f(_lastTrigPk / _lastAmb);
}

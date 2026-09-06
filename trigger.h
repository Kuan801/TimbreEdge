#pragma once

#include <stdint.h>
#include "config.h"

#define TC_BLOCK_TOPK 4
float tcBlockLevel(const int16_t *src, int n);

class TriggerGate {
public:

  void arm(float baseThreshold, uint32_t nowMs);

  bool feed(float blockPeak, uint32_t nowMs);

  void noteRecorded(uint32_t nowMs);

  bool  calibrating() const { return _calibrating; }
  float ambient()     const { return _ambient; }
  float threshold()   const { return _thresh;  }
  float level()       const { return _level;   }
  bool  armedReady()  const { return _rearmed; }

  int   calSpikes()   const { return _calSpikes; }

  float lastHeadroomDb() const;

  float ambientRatio() const { return _base > 0 ? _thresh / _base : 1.0f; }

private:

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

// ============================================================================
//  player.h  -  score scheduler
//  Polled from loop(); drives noteOn / noteOff on the synth along a tick timeline.
//  Time is accumulated with micros(), so it won't drift when loop() stalls on SD access.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "score.h"
#include "additive_synth.h"

class Player {
public:
  void begin(AudioSynthAdditive *synth);
  void load();                                  // Regenerate the score (playback is stopped first)
  void start(float bpm = TC_BPM);
  void stop();
  void service();

  bool  playing() const { return _playing; }
  float progress() const {
    return _totalTicks ? tc_clampf((float)_tick / _totalTicks, 0.f, 1.f) : 0.f;
  }

private:
  struct Pending { uint16_t offTick; uint8_t midi; bool used; };

  AudioSynthAdditive *_synth = nullptr;
  ScoreNote  _notes[TC_MAX_NOTES];
  int        _n          = 0;
  int        _cursor     = 0;
  Pending    _pend[TC_N_VOICES * 4];

  bool       _playing    = false;
  uint16_t   _tick       = 0;
  uint16_t   _totalTicks = 0;
  uint32_t   _tickUs     = 0;
  uint32_t   _lastUs     = 0;
  uint32_t   _accUs      = 0;

  void scheduleOff(uint16_t offTick, uint8_t midi);
  void fireTick();
};

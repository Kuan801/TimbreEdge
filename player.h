// ============================================================================
//  player.h  -  樂譜排程器
//  在 loop() 裡輪詢，依 tick 時間軸對合成器下 noteOn / noteOff。
//  用 micros() 累積時間，不會因為 loop 被 SD 存取卡住而漂移。
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "score.h"
#include "additive_synth.h"

class Player {
public:
  void begin(AudioSynthAdditive *synth);
  void load();                                  // 重新產生樂譜（演奏中會先停）
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

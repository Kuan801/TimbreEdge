#pragma once

#include <Arduino.h>
#include "config.h"
#include "additive_synth.h"

#define TC_N_KEYS 12

class Keys {
public:
  void begin(AudioSynthAdditive *synth);

  void service();

  void setEnabled(bool on);
  bool enabled() const { return _on; }

  void setTranspose(int8_t semi);
  int8_t transpose() const { return _transpose; }

  uint16_t downMask() const { return _mask; }
  int      downCount() const;

private:
  AudioSynthAdditive *_synth = nullptr;
  bool     _on = false;
  int8_t   _transpose = 0;
  uint16_t _mask = 0;

  struct K {
    uint8_t  pin;
    bool     stable  = false;
    bool     raw     = false;
    uint32_t changed = 0;
    uint8_t  playing = 0;
  };
  K _k[TC_N_KEYS];
};

extern Keys gKeys;

void keysDownText(char *out, size_t cap);

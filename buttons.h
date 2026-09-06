#pragma once

#include <Arduino.h>
#include "config.h"
#include "ui.h"

class Buttons {
public:
  void begin();

  UiKey poll();

  bool anyDown() const;

private:
  struct Btn {
    uint8_t  pin;
    UiKey    key;
    bool     stable   = false;
    bool     raw      = false;
    uint32_t changed  = 0;
    uint32_t downAt   = 0;
    uint32_t lastRep  = 0;
    bool     repeats  = false;
  };
  Btn _b[4];
};

extern Buttons gButtons;

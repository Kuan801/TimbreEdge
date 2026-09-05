// ============================================================================
//  buttons.h  -  debouncing and edge detection for the 4 tactile switches
//
//  Why this is separate from ui.h: this is the only part that touches hardware
//  (digitalRead / millis), while ui.cpp is a pure state machine. Split that
//  way, the menu logic is testable on the desktop.
//
//  Wiring: one leg of the button to a Teensy pin, the other to GND. With
//  INPUT_PULLUP no external resistor is needed, and a press reads LOW.
//
//  Debouncing works by "only accept a state once it has held for
//  TC_BTN_DEBOUNCE_MS" rather than delay() on every press. Tactile switches
//  usually bounce for 1~5 ms, longer with a poor contact; a delay would stall
//  the audio loop, and this program's loop also has to feed USB Host and the
//  SD writes.
//
//  Up/down auto-repeat (every 120 ms after being held for 400 ms) so that
//  setting a 0~63 value like micGain does not take 63 presses.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "ui.h"

class Buttons {
public:
  void begin();

  // Call from loop(). Returns this round's key event (one at a time), else UI_KEY_NONE.
  UiKey poll();

  // Is any button currently held down (used by the power-on self test)
  bool anyDown() const;

private:
  struct Btn {
    uint8_t  pin;
    UiKey    key;
    bool     stable   = false;   // Debounced state, true = pressed
    bool     raw      = false;
    uint32_t changed  = 0;       // When raw last changed
    uint32_t downAt   = 0;       // When stable became pressed
    uint32_t lastRep  = 0;       // Time of the last auto-repeat
    bool     repeats  = false;   // Whether this button auto-repeats
  };
  Btn _b[4];
};

extern Buttons gButtons;

// ============================================================================
//  keys.h  -  12 physical piano keys (C4 ~ B4)
//
//  Wiring: one leg of each button to its pin, the other to GND. Internal pull-ups,
//  so no external resistors. All 12 share one GND wire, 13 wires in total.
//
//        White keys (the near row, same side as 0~12)
//           C4=24   D4=25   E4=26   F4=27   G4=28   A4=29   B4=30
//        Black keys (the far row, same side as 13~23)
//           C#4=34  D#4=35  F#4=36  G#4=37  A#4=38
//
//  The tail of the Teensy 4.1 is already split into two rows (near 24~33, far
//  34~41), so they are divided the way a real keyboard is laid out: the upper row
//  all black keys, the lower row all white keys, each group contiguous, so a wire
//  in the wrong place is obvious at a glance.
//
//  This stretch is out of the audio shield's reach, so it does not conflict with
//  the shield at all. It ships without headers soldered on, though — see
//  "wiring the free tail" in the README for how to connect it.
//
//  --- Why 12 pins directly instead of a 4x3 matrix ----------------------------
//
//  A matrix needs only 7 pins, but pressing three or more keys at once produces
//  ghost keys — notes sound that were never pressed. Avoiding that needs a diode
//  in series with every button: 12 buttons, 12 diodes.
//  The Teensy 4.1 has 21 free pins, more than enough to wire them directly, and
//  then chords really work. This synth has 8 voices, so 8 keys at once all sound —
//  with a matrix that would be wasted.
//
//  --- Why this is separate from buttons.h ------------------------------------
//
//  The menu buttons want "one press, one event" plus auto-repeat on hold; the
//  piano keys want "press sounds, release damps", and they have to work
//  simultaneously. The two semantics differ, and forcing them into one class only
//  makes both sides awkward.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "additive_synth.h"

#define TC_N_KEYS 12

class Keys {
public:
  void begin(AudioSynthAdditive *synth);

  // Call from loop(). It only actually sounds when enabled().
  void service();

  // Enter / leave key mode. Leaving cleanly releases any notes still held.
  void setEnabled(bool on);
  bool enabled() const { return _on; }

  // Transpose (used by the octave buttons). In semitones; 0 = C4~B4 as-is.
  void setTranspose(int8_t semi);
  int8_t transpose() const { return _transpose; }

  // For the OLED display: which keys are currently held (bit 0 = C4)
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
    uint8_t  playing = 0;    // the MIDI pitch actually sent (release has to use the same one to turn it off)
  };
  K _k[TC_N_KEYS];
};

extern Keys gKeys;

// lay the held keys out as a string like "C E G", for the OLED
void keysDownText(char *out, size_t cap);

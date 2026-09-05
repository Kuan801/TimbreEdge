// ============================================================================
//  midi_in.h  -  USB Host MIDI keyboard input
//
//  The row of 5 pins on the underside of the Teensy 4.1 is an independent USB
//  Host port, completely separate from the Micro-USB used for flashing and the
//  serial monitor, so a keyboard can be plugged in and played while Serial
//  messages are still readable.
//
//  Wiring (Teensy 4.1 underside, the 5 pins near the SD card slot):
//      5V  -> USB A socket pin 1 (VBUS, red)
//      D-  -> pin 2 (white)
//      D+  -> pin 3 (green)
//      GND -> pin 4 (black)
//      Pin 5 is shield; connect it to the socket's metal shell, or leave it off
//  Do not swap the order: with D+/D- reversed the device is never enumerated at
//  all, and there is no error message of any kind.
//
//  Power note: 5V on this port comes from VUSB, through the current-limiting
//  switch on the Teensy. If you power the board externally (VIN), you must cut
//  the VUSB-VIN trace on the back, or you will backfeed power into the computer's
//  USB port. The Keystation Mini 32 is a low-power bus-powered device, so it can
//  be connected directly while the computer supplies power.
//
//  Why this is a separate module rather than being folded into the .ino:
//  the desktop simulator has no USBHost_t36, and isolating it here means a single
//  TC_USE_USB_MIDI switch turns the whole thing off -- the simulator still
//  compiles and not one line of other code has to change.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "additive_synth.h"

// These numbers match the factory settings of the M-AUDIO Keystation Mini 32 MK3.
// They will usually work with other keyboards too, since they are standard MIDI CCs.
#define TC_MIDI_CC_MOD      1     // MOD button
#define TC_MIDI_CC_VOLUME   7     // Volume knob
#define TC_MIDI_CC_SUSTAIN 64     // SUST button
#define TC_MIDI_CC_ALLOFF 123

// Full-scale pitch bend range (semitones). The standard MIDI convention is ±2.
#define TC_MIDI_BEND_RANGE  2.0f
// How much extra vibrato (cents) at full modulation wheel
#define TC_MIDI_MOD_CENTS  35.0f

class MidiInput {
public:
  void begin(AudioSynthAdditive *s);

  // Call as often as possible from loop(). USB Host enumeration and packet
  // reception both happen in here.
  void service();

  // Per-stage counters. When "I pressed a key and nothing happened", these have to
  // say which stage it broke at; guessing never finishes.
  void report() const;
  void setVerbose(bool v) { _verbose = v; }
  bool verbose() const { return _verbose; }

  bool        connected()   const { return _connected; }
  const char *deviceName()  const { return _name; }
  uint32_t    noteCount()   const { return _notes; }
  int         heldNotes()   const { return _nHeld; }

  // Call before a long blocking flow such as analysis or training, so no note is
  // left stuck sounding
  void panic();

  // Feed in one MIDI message. This is what service() calls once it has parsed USB.
  //
  // Deliberately public, for two reasons:
  //   1) This logic (sustain pedal, velocity curve, pitch bend) has nothing to do
  //      with USB, so pulling it out makes it testable in the desktop simulator
  //      without plugging in a real keyboard.
  //   2) Adding DIN-5 MIDI or USB device MIDI later is then just another source
  //      feeding into the same entry point.
  void feed(uint8_t status, uint8_t d1, uint8_t d2);

private:
  void onNoteOn(uint8_t note, uint8_t vel);
  void onNoteOff(uint8_t note);
  void onControl(uint8_t cc, uint8_t val);

  AudioSynthAdditive *_synth = nullptr;
  bool     _connected = false;
  char     _name[48]  = "";
  uint32_t _notes     = 0;      // NoteOn messages received
  uint32_t _msgs      = 0;      // All MIDI messages received
  uint32_t _ccs       = 0;      // Control Change messages received
  uint32_t _sounded   = 0;      // Notes that actually produced sound
  uint32_t _noTimbre  = 0;      // Notes dropped because no timbre was loaded
  uint32_t _noVoice   = 0;      // Notes dropped because no voice could be allocated
  uint32_t _rawReads  = 0;      // Times the low-level read() successfully decoded a message
  int      _nPorts    = 0;      // Number of USB interfaces claimed
  bool     _verbose   = false;
  bool     _warned    = false;  // Print the "no timbre" warning once, do not flood the log

  // Sustain pedal: while it is held, NoteOff is recorded and only actually sent
  // when the pedal is released
  bool     _sustain = false;
  uint8_t  _held[16];          // Notes already released during the pedal hold, waiting to be cut
  int      _nDeferred = 0;
  int      _nHeld     = 0;     // How many keys are physically held right now (for the panel)
};

extern MidiInput gMidi;

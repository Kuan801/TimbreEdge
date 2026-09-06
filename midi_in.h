#pragma once

#include <Arduino.h>
#include "config.h"
#include "additive_synth.h"

#define TC_MIDI_CC_MOD      1
#define TC_MIDI_CC_VOLUME   7
#define TC_MIDI_CC_SUSTAIN 64
#define TC_MIDI_CC_ALLOFF 123

#define TC_MIDI_BEND_RANGE  2.0f

#define TC_MIDI_MOD_CENTS  35.0f

class MidiInput {
public:
  void begin(AudioSynthAdditive *s);

  void service();

  void report() const;
  void setVerbose(bool v) { _verbose = v; }
  bool verbose() const { return _verbose; }

  bool        connected()   const { return _connected; }
  const char *deviceName()  const { return _name; }
  uint32_t    noteCount()   const { return _notes; }
  int         heldNotes()   const { return _nHeld; }

  void panic();

  void feed(uint8_t status, uint8_t d1, uint8_t d2);

private:
  void onNoteOn(uint8_t note, uint8_t vel);
  void onNoteOff(uint8_t note);
  void onControl(uint8_t cc, uint8_t val);

  AudioSynthAdditive *_synth = nullptr;
  bool     _connected = false;
  char     _name[48]  = "";
  uint32_t _notes     = 0;
  uint32_t _msgs      = 0;
  uint32_t _ccs       = 0;
  uint32_t _sounded   = 0;
  uint32_t _noTimbre  = 0;
  uint32_t _noVoice   = 0;
  uint32_t _rawReads  = 0;
  int      _nPorts    = 0;
  bool     _verbose   = false;
  bool     _warned    = false;

  bool     _sustain = false;
  uint8_t  _held[16];
  int      _nDeferred = 0;
  int      _nHeld     = 0;
};

extern MidiInput gMidi;

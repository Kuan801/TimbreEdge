// ============================================================================
//  recorder.h  -  record N seconds from the Audio Shield microphone, streaming
//                 straight into a WAV on the SD card
//  Approach: AudioRecordQueue hands us a 128-sample block every 2.9 ms; we
//            accumulate 512 bytes before writing to SD, so SD write latency
//            cannot cause dropouts.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Audio.h>
#include "config.h"
#include "wav_io.h"
#include "trigger.h"

class Recorder {
public:
  void begin(AudioRecordQueue *q) { _q = q; }

  // Start recording (non-blocking). Stops automatically after `seconds`.
  bool start(const char *path = TC_REC_PATH, uint32_t seconds = TC_REC_SECONDS);

  // Must be called frequently from loop(). Returns true once this call has
  // finished the recording.
  bool service();

  bool  active()   const { return _active; }
  float progress() const { return _target ? (float)_written / (float)_target : 0.0f; }
  float peak()     const { return _peak; }

  // ---------------------------------- continuous sampling (auto-trigger) --
  //
  // One person holding an instrument cannot also press keys, so this mode
  // records by itself as soon as it hears something.
  //
  // The key part is the pre-roll: by the time the level crosses the threshold the
  // onset has already happened. Without a pre-buffer the most important part, the
  // attack transient, is cut off -- and the attack is the main cue the ear uses to
  // identify an instrument. This keeps a fixed 93 ms before the trigger.
  void armSession(float threshold = TC_TRIG_LEVEL);
  void setThreshold(float t) { _thresh = t; }

  // The threshold actually in force (which room noise may have raised) and the
  // measured room noise itself. The UI has to show both -- showing only the
  // threshold leaves the user unable to tell whether they set it or the room did.
  float threshold()   const { return _gate.threshold(); }
  float baseThresh()  const { return _thresh; }
  float ambient()     const { return _gate.ambient(); }
  bool  calibrating() const { return _gate.calibrating(); }
  float headroomDb()  const { return _gate.lastHeadroomDb(); }

  // Monitor only: read the level without recording, to confirm whether the
  // microphone is producing any signal at all
  void beginMonitor();
  void endMonitor();
  bool monitoring() const { return _monitor; }
  void serviceMonitor();
  float monPeak() const { return _monPeak; }
  float monRms()  const { return _monRms;  }
  // DC offset, and the AC RMS with the DC removed.
  //
  // They are reported separately because the fixes are completely different: low
  // AC = a pickup problem (gain, wiring, the microphone itself); high DC = the
  // codec is not subtracting DC, which is a configuration problem.
  // Looking only at total RMS, a 0.026 DC pedestal looks like "there is signal",
  // and it also raises the trigger threshold -- but the symptom presents as
  // "the microphone is not sensitive".
  float monDc()   const { return _monDc;   }
  float monAc()   const { return _monAc;   }
  void  monReset() { _monPeak = 0.0f; }

  // Per-band RMS for three bands: low (<300 Hz), mid (300~2k), high (>2k)
  //
  // Why this is needed: measurement showed that a piano picked up by the
  // microphone had almost no fundamental, with the frequency response 33 dB above
  // the reference material at 2~4 kHz. Total level says nothing about this --
  // the total is normal, but all the energy is up high.
  //
  // Split into three bands, you can move the microphone and watch the low band
  // come back, instead of having to finish the take, copy it to a computer and run
  // chaincheck before you know.
  float monLow()  const { return _monLow;  }
  float monMid()  const { return _monMid;  }
  float monHigh() const { return _monHigh; }
  void endSession();
  bool sessionOn()  const { return _session; }
  bool armed()      const { return _session && !_active; }
  float level()     const { return _gate.level(); }  // Live level for the UI

private:
  AudioRecordQueue *_q       = nullptr;
  WavWriter         _w;
  bool              _active  = false;
  uint32_t          _written = 0;      // Samples written so far
  uint32_t          _hdrAt   = 0;      // Samples written as of the last header patch
  uint32_t          _target  = 0;
  float             _peak    = 0.0f;
  int16_t           _buf[256];         // 2 audio blocks
  uint16_t          _fill    = 0;

  // Continuous-sampling state
  bool     _session   = false;
  float    _thresh    = TC_TRIG_LEVEL;
  TriggerGate _gate;
  int16_t  _pre[TC_PREROLL_BLOCKS * TC_BLOCK];    // Ring pre-buffer
  int      _preHead   = 0;
  int      _preCount  = 0;

  bool     _monitor  = false;
  float    _monPeak  = 0.0f;
  float    _monRms   = 0.0f;
  float    _monDc    = 0.0f;
  float    _monAc    = 0.0f;
  // For the three-band measurement. lp1 = 300 Hz one-pole low-pass, lp2 = 2 kHz
  // one-pole low-pass. Low = lp1, high = signal - lp2, mid = lp2 - lp1.
  // One-pole filters do not separate the bands sharply (only 6 dB per octave), but
  // the goal here is "can you see that the low end is badly missing", not precise
  // spectral analysis, so it is good enough.
  float    _lp1 = 0.0f, _lp2 = 0.0f;
  float    _monLow = 0.0f, _monMid = 0.0f, _monHigh = 0.0f;

  void serviceArmed();
  bool startFromTrigger();
};

// ============================================================================
//  StereoCapture  -  record the exact signal the synthesizer sends to the
//                    headphones as a stereo WAV
//
//  It taps in after the output mixer, so what is recorded is what is heard:
//  additive synthesis + reverb, with no sample playback mixed in anywhere.
//  Written to SD in step with the canon performance.
//
//  44.1k stereo 16-bit = 176 KB/s. The Teensy 4.1 built-in SDIO handles that
//  easily; the Audio Shield's SPI slot may drop frames, and dropped frames are
//  reported.
// ============================================================================
class StereoCapture {
public:
  void begin(AudioRecordQueue *qL, AudioRecordQueue *qR) { _qL = qL; _qR = qR; }

  bool start(const char *path = TC_PLAY_PATH);
  void service();                      // Call frequently from loop()
  void stop();

  bool     active()  const { return _active; }
  uint32_t frames()  const { return _frames; }
  uint32_t dropped() const { return _dropped; }
  float    seconds() const { return _frames / TC_SAMPLE_RATE; }

private:
  AudioRecordQueue *_qL = nullptr, *_qR = nullptr;
  WavWriter _w;
  bool      _active  = false;
  uint32_t  _frames  = 0;
  uint32_t  _dropped = 0;
  // Which frame we had reached the last time the WAV header length was patched.
  // Patching periodically is what keeps a file left behind by power loss / reset /
  // pulling the card before stop a valid WAV (see wav_io.cpp).
  uint32_t  _hdrAt   = 0;
  int16_t   _buf[512];                 // 256 stereo samples = 2 audio blocks
  uint16_t  _fill    = 0;
};

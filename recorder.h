#pragma once

#include <Arduino.h>
#include <Audio.h>
#include "config.h"
#include "wav_io.h"
#include "trigger.h"

class Recorder {
public:
  void begin(AudioRecordQueue *q) { _q = q; }

  bool start(const char *path = TC_REC_PATH, uint32_t seconds = TC_REC_SECONDS);

  bool service();

  bool  active()   const { return _active; }
  float progress() const { return _target ? (float)_written / (float)_target : 0.0f; }
  float peak()     const { return _peak; }

  void armSession(float threshold = TC_TRIG_LEVEL);
  void setThreshold(float t) { _thresh = t; }

  float threshold()   const { return _gate.threshold(); }
  float baseThresh()  const { return _thresh; }
  float ambient()     const { return _gate.ambient(); }
  bool  calibrating() const { return _gate.calibrating(); }
  float headroomDb()  const { return _gate.lastHeadroomDb(); }

  void beginMonitor();
  void endMonitor();
  bool monitoring() const { return _monitor; }
  void serviceMonitor();
  float monPeak() const { return _monPeak; }
  float monRms()  const { return _monRms;  }

  float monDc()   const { return _monDc;   }
  float monAc()   const { return _monAc;   }
  void  monReset() { _monPeak = 0.0f; }

  float monLow()  const { return _monLow;  }
  float monMid()  const { return _monMid;  }
  float monHigh() const { return _monHigh; }
  void endSession();
  bool sessionOn()  const { return _session; }
  bool armed()      const { return _session && !_active; }
  float level()     const { return _gate.level(); }

private:
  AudioRecordQueue *_q       = nullptr;
  WavWriter         _w;
  bool              _active  = false;
  uint32_t          _written = 0;
  uint32_t          _hdrAt   = 0;
  uint32_t          _target  = 0;
  float             _peak    = 0.0f;
  int16_t           _buf[256];
  uint16_t          _fill    = 0;

  bool     _session   = false;
  float    _thresh    = TC_TRIG_LEVEL;
  TriggerGate _gate;
  int16_t  _pre[TC_PREROLL_BLOCKS * TC_BLOCK];
  int      _preHead   = 0;
  int      _preCount  = 0;

  bool     _monitor  = false;
  float    _monPeak  = 0.0f;
  float    _monRms   = 0.0f;
  float    _monDc    = 0.0f;
  float    _monAc    = 0.0f;

  float    _lp1 = 0.0f, _lp2 = 0.0f;
  float    _monLow = 0.0f, _monMid = 0.0f, _monHigh = 0.0f;

  void serviceArmed();
  bool startFromTrigger();
};

class StereoCapture {
public:
  void begin(AudioRecordQueue *qL, AudioRecordQueue *qR) { _qL = qL; _qR = qR; }

  bool start(const char *path = TC_PLAY_PATH);
  void service();
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

  uint32_t  _hdrAt   = 0;
  int16_t   _buf[512];
  uint16_t  _fill    = 0;
};

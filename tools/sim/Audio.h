// tools/sim/Audio.h  -  minimal Teensy Audio Library shim (just enough for the synth)
#pragma once

#include "Arduino.h"

#define AUDIO_BLOCK_SAMPLES 128

struct audio_block_t {
  int16_t data[AUDIO_BLOCK_SAMPLES];
  bool    inUse;
};

// Simulator: whatever gets transmitted lands here
extern int16_t sim_outL[AUDIO_BLOCK_SAMPLES];
extern int16_t sim_outR[AUDIO_BLOCK_SAMPLES];

class AudioStream {
public:
  AudioStream(unsigned char, void *) {}
  virtual void update(void) = 0;
  virtual ~AudioStream() {}
protected:
  audio_block_t *allocate();
  void release(audio_block_t *b);
  void transmit(audio_block_t *b, unsigned char ch = 0);
};

// SPI / Wire are never used; empty shims
class SPIClass { public: void setMOSI(int) {} void setSCK(int) {} };
extern SPIClass SPI;

// ============================================================================
//  The rest is only for `make inocheck`: syntax-check TimbreClone.ino on the
//  desktop.
//
//  The simulator itself does not use these classes (it calls
//  AudioSynthAdditive::update() directly, bypassing the audio graph), but the
//  audio graph declarations in the .ino need them to exist.
//
//  Why it is worth doing: the .ino is the only file with no desktop compile
//  coverage, and it has already let two errors through -- once a menu page
//  count that did not match, once gUiOctave declared after its use site.
//  Arduino only auto-inserts function prototypes, not variable declarations,
//  so that kind of error only blows up when you flash.
//
//  This only guarantees "it compiles"; the behaviour is completely empty, so
//  do not run anything on it.
// ============================================================================
#ifdef TC_INO_CHECK

#define AUDIO_INPUT_MIC     0
#define AUDIO_INPUT_LINEIN   1

// All of them must derive from AudioStream, or AudioConnection cannot hook them up
class TcStubStream : public AudioStream {
public:
  TcStubStream() : AudioStream(0, nullptr) {}
  void update(void) override {}
};

class AudioInputI2S  : public TcStubStream {};
class AudioOutputI2S : public TcStubStream {};
class AudioEffectFreeverbStereo : public TcStubStream {
public:
  void roomsize(float) {} void damping(float) {}
};
class AudioMixer4 : public TcStubStream { public: void gain(int, float) {} };
class AudioRecordQueue : public TcStubStream {
public:
  void begin() {} void end() {} void clear() {}
  int  available() { return 0; }
  int16_t *readBuffer() { return nullptr; }
  void freeBuffer() {}
};
class AudioConnection {
public:
  AudioConnection(AudioStream &, AudioStream &) {}
  AudioConnection(AudioStream &, unsigned char, AudioStream &, unsigned char) {}
};
// The return types have to match the real library (PJRC control_sgtl5000.h: these
// are all bool, returning "did the I2C write succeed"). This used to be void, so
// the moment the .ino started checking return values, inocheck would block it with
// "void value not ignored" -- that is a sign of the fake header drifting from the
// real one, not of a bug in the .ino.
class AudioControlSGTL5000 {
public:
  bool enable() { return true; }
  bool volume(float) { return true; }
  bool inputSelect(int) { return true; }
  bool micGain(unsigned int) { return true; }
  bool lineInLevel(unsigned char) { return true; }
  unsigned short adcHighPassFilterDisable() { return 0; }
  unsigned short adcHighPassFilterEnable() { return 0; }
  unsigned short adcHighPassFilterFreeze() { return 0; }
};
inline void AudioMemory(int) {}
inline float AudioProcessorUsage() { return 0.0f; }
inline float AudioProcessorUsageMax() { return 0.0f; }
inline void  AudioProcessorUsageMaxReset() {}
inline int   AudioMemoryUsageMax() { return 0; }

#endif  // TC_INO_CHECK

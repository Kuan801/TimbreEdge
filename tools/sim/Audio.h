#pragma once

#include "Arduino.h"

#define AUDIO_BLOCK_SAMPLES 128

struct audio_block_t {
  int16_t data[AUDIO_BLOCK_SAMPLES];
  bool    inUse;
};

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

class SPIClass { public: void setMOSI(int) {} void setSCK(int) {} };
extern SPIClass SPI;

#ifdef TC_INO_CHECK

#define AUDIO_INPUT_MIC     0
#define AUDIO_INPUT_LINEIN   1

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

#endif

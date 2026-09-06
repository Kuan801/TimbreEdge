#pragma once

#include <Arduino.h>
#include <SD.h>
#include "config.h"

bool tcSdBegin();

void tcSdList();

int tcSdCollectWavs(const char *dir, char *outNames, int maxCount,
                    const char *skipName = nullptr);

bool tcSdCopy(const char *src, const char *dst);

#define TC_SET_PREFIX      "SET"
#define TC_SET_MAX         99

bool tcSdMakeNextSet(char *out, size_t cap);

bool tcIsSetDir(const char *name);

int tcSdCollectSets(char *outNames, int maxCount);

bool tcSdRemoveDir(const char *dir, int *deletedFiles = nullptr,
                   uint32_t *freedBytes = nullptr);

class WavReader {
public:
  bool     open(const char *path);
  void     close();
  bool     isOpen() const { return _open; }

  uint32_t frames()     const { return _frames; }
  uint32_t sampleRate() const { return _sampleRate; }
  uint16_t channels()   const { return _channels; }

  uint32_t readMono(uint32_t frameIndex, float *dst, uint32_t n);

private:
  File     _f;
  bool     _open       = false;
  uint32_t _dataOffset = 0;
  uint32_t _frames     = 0;
  uint32_t _sampleRate = 44100;
  uint16_t _channels   = 1;
  uint16_t _bits       = 16;
};

class WavWriter {
public:

  bool open(const char *path, uint32_t sampleRate = 44100, uint16_t channels = 1,
            uint32_t expectedSamples = 0);
  bool writeSamples(const int16_t *src, uint32_t n);
  void close();

  void flushHeader();
  bool isOpen() const { return _open; }
  uint32_t bytesWritten() const { return _dataBytes; }

private:
  File     _f;
  bool     _open      = false;
  uint32_t _dataBytes = 0;
  uint32_t _sampleRate = 44100;
  uint16_t _channels   = 1;
};

bool tcIsGeneratedWav(const char *name);

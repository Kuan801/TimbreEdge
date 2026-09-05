// ============================================================================
//  wav_io.h  -  minimal 16-bit PCM WAV read/write (mono / stereo, downmixed to mono on read)
// ============================================================================
#pragma once

#include <Arduino.h>
#include <SD.h>
#include "config.h"

// SD init: try the Teensy 4.1 built-in SDIO first, fall back to the Audio Shield's SPI slot
bool tcSdBegin();

// List the root directory. For diagnostics: tells "the SD did not mount" apart from "the file is not on the card" at a glance.
void tcSdList();

// Scan a directory for every .WAV, collecting the names into outNames (TC_MAX_NAME_LEN bytes each).
// Pass "/" as dir for the root directory. Returns how many were found.
// A non-NULL skipName skips that file (e.g. excluding the just-recorded REC.WAV during a batch load).
int tcSdCollectWavs(const char *dir, char *outNames, int maxCount,
                    const char *skipName = nullptr);

// Copy a file (sampling mode saves REC.WAV as a pitch-named file like A4.WAV)
bool tcSdCopy(const char *src, const char *dst);

// ------------------------------------------------- sampling folders (SETnn) --
//
// Every continuous sampling run opens a new folder (SET01, SET02, ...) and all of that run's WAVs go in it.
//
// Why separate them: everything used to pile up in the root, so the second run's C4.WAV
// simply overwrote the first one's, and "n *" would train on material from different
// instruments and different runs all mixed together, with no way to tell from the outside.
// With folders, one run is one set, and choosing a set is just "n SET01/".
//
// Why a running number and not a timestamp: the Teensy 4.1 RTC only runs with a coin cell
// fitted. Without one, every boot starts from the same time, so timestamps would overwrite
// each other -- worse than a running number. A running number depends only on which folders
// are already on the card, so it is always right.

#define TC_SET_PREFIX      "SET"
#define TC_SET_MAX         99

// Find the next unused SETnn and create it. On success the name is written to out (e.g. "SET03").
// Returns false when SET01~SET99 already exist on the card.
bool tcSdMakeNextSet(char *out, size_t cap);

// Whether this name is a sampling folder (SET prefix + two digits).
// Pure string logic, testable on the desktop.
bool tcIsSetDir(const char *name);

// Scan the card for every sampling folder, sorted by name. Returns how many were found.
int tcSdCollectSets(char *outNames, int maxCount);

// Delete a whole folder (the files inside first, then the directory itself).
// When non-NULL, deletedFiles / freedBytes report how many files went and how much space was freed.
bool tcSdRemoveDir(const char *dir, int *deletedFiles = nullptr,
                   uint32_t *freedBytes = nullptr);

// ------------------------------------------------------------------ reader --
class WavReader {
public:
  bool     open(const char *path);
  void     close();
  bool     isOpen() const { return _open; }

  uint32_t frames()     const { return _frames; }      // Sample frames per channel
  uint32_t sampleRate() const { return _sampleRate; }
  uint16_t channels()   const { return _channels; }

  // Read n frames starting at frame frameIndex (mono float, -1..1). Returns how many were actually read.
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

// ------------------------------------------------------------------ writer --
class WavWriter {
public:
  // With expectedSamples > 0 the correct length goes into the header up front, so the file
  // is still a valid WAV even where the SD implementation forbids seeking back to patch it.
  bool open(const char *path, uint32_t sampleRate = 44100, uint16_t channels = 1,
            uint32_t expectedSamples = 0);
  bool writeSamples(const int16_t *src, uint32_t n);   // n = number of int16 samples
  void close();                                        // Seek back and fix up the RIFF length
  // Patch the RIFF/data lengths to their correct value so far while still recording,
  // then flush to the card. Why this is needed: if the lengths are only patched in
  // close(), a file left behind by "power lost / reset / card pulled before stop was
  // pressed" has a data length of 0 -- Windows declares it corrupt and refuses to play
  // it, even with several MB of audio already inside. See the notes in wav_io.cpp.
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

// Whether this filename is a WAV the program generated itself (REC.WAV / PLAY.WAV /
// a plain note-name file).
//
// It lives here rather than in the .ino because it is the test used for deleting files --
// wrongly deleting material the user worked hard to record cannot be undone, so a rule
// like this has to be testable. See tools/sim/wavname_test.cpp.
bool tcIsGeneratedWav(const char *name);

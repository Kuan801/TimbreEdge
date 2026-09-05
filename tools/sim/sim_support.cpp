// ============================================================================
//  sim_support.cpp  -  platform glue for the desktop simulator
//
//  Stub implementations of Serial / SD / SPI / AudioStream. These used to live
//  next to main() in sim_main.cpp; splitting them out is what lets extra test
//  programs such as midi_test link against them.
// ============================================================================
#include "Arduino.h"
#include "Audio.h"
#include "SD.h"

#include <string>
#include <cstring>

// ------------------------------------------------- global simulator state --
uint64_t    sim_micros = 0;
SimSerial   Serial;
SDClass     SD;
SPIClass    SPI;
std::string sim_sd_root = ".";
int16_t     sim_outL[AUDIO_BLOCK_SAMPLES];
int16_t     sim_outR[AUDIO_BLOCK_SAMPLES];

static audio_block_t sPool[16];

audio_block_t *AudioStream::allocate() {
  for (auto &b : sPool)
    if (!b.inUse) { b.inUse = true; return &b; }
  return nullptr;
}
void AudioStream::release(audio_block_t *b) { if (b) b->inUse = false; }
void AudioStream::transmit(audio_block_t *b, unsigned char ch) {
  if (!b) return;
  memcpy(ch == 0 ? sim_outL : sim_outR, b->data, sizeof(b->data));
}


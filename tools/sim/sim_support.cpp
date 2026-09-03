// ============================================================================
//  sim_support.cpp  -  桌機模擬器的平台膠水
//
//  Serial / SD / SPI / AudioStream 的空殼實作。原本跟 main() 綁在
//  sim_main.cpp 裡，抽出來之後 midi_test 這類額外的測試程式才連結得到。
// ============================================================================
#include "Arduino.h"
#include "Audio.h"
#include "SD.h"

#include <string>
#include <cstring>

// ------------------------------------------------------- 全域模擬狀態 ------
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


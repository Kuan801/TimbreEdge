// ============================================================================
//  xrange.cpp  -  experiment tool: "train on a narrow range, extrapolate out"
//
//  Usage:
//     ./xrange out.wav <startMIDI> <endMIDI> training.WAV [training.WAV ...]
//
//  Example: build the bank from C4~B4 only, but make it play E3~B5
//     ./xrange out.wav 52 83 T_C4.WAV T_Db4.WAV ... T_B4.WAV
//
//  Why this exists instead of just using sim:
//  sim plays the 24 notes C3~B4 hard-coded in score.cpp. To measure "how far
//  transposition degrades the timbre" the played range and the trained range
//  have to be given independently -- notes inside the trained range are
//  interpolation, only the ones outside really exercise transposition and
//  formant correction.
//
//  Timing is deliberately identical to score.cpp (66 BPM, 8 ticks per note,
//  no gaps) so that evaluate.py can cut the notes apart.
// ============================================================================
#include <Arduino.h>
#include <Audio.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../config.h"
#include "../../profile.h"
#include "../../analyzer.h"
#include "../../timbre_model.h"
#include "../../additive_synth.h"

static ProfileBank        gBank;
static TimbreModel        gModel;
static AudioSynthAdditive gSynth;

static const char *kNames[12] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};

int main(int argc, char **argv) {
  if (argc < 5) {
    printf("用法：./xrange out.wav <起始MIDI> <結束MIDI> 訓練用.WAV [...]\n");
    printf("      例：./xrange out.wav 52 83 T_C4.WAV ... T_B4.WAV\n");
    return 1;
  }
  const char *outPath = argv[1];
  const int   midiLo  = atoi(argv[2]);
  const int   midiHi  = atoi(argv[3]);

  // ---- Build the bank (only the files given on the command line) ----------
  printf("=== 建立音色庫 ===\n");
  for (int i = 4; i < argc; i++) {
    InstrumentProfile p;
    if (analyzeWavFile(argv[i], p, nullptr, nullptr)) {
      p.valid = true;
      gBank.add(p);
    } else {
      printf("  ! %s 分析失敗\n", argv[i]);
    }
  }
  if (gBank.n == 0) { printf("沒有可用的素材\n"); return 1; }
  gModel.setBank(&gBank);
  gModel.setProfile(&gBank.p[0]);
  gSynth.setModel(&gModel);
  gSynth.setMasterGain(0.18f);
  gSynth.setVibrato(50.0f, 4.8f);

  // Pitch range the bank covers (used to mark each note interpolated or extrapolated)
  float bankLo = 1e9f, bankHi = 0.0f;
  for (int i = 0; i < gBank.n; i++) {
    if (gBank.p[i].f0 < bankLo) bankLo = gBank.p[i].f0;
    if (gBank.p[i].f0 > bankHi) bankHi = gBank.p[i].f0;
  }
  printf("\n音色庫 %d 組，涵蓋 %.1f ~ %.1f Hz\n", gBank.n, bankLo, bankHi);

  // ---- Play ---------------------------------------------------------------
  // Timing matches score.cpp: 8 ticks per note, tick = 60/(BPM*4) seconds
  const float tickSec  = 60.0f / (TC_BPM * TC_TICKS_PER_BEAT);
  const float noteSec  = 8.0f * tickSec;
  const int   blocksPerNote = (int)(noteSec / TC_BLOCK_SEC + 0.5f);

  std::vector<int16_t> pcm;
  printf("\n=== 演奏 %d 個音 ===\n", midiHi - midiLo + 1);
  printf("%6s %8s %10s %12s\n", "音", "f0(Hz)", "用哪一組", "移調量");

  for (int m = midiLo; m <= midiHi; m++) {
    const float hz = tc_midiToHz((float)m);
    const InstrumentProfile *p = gModel.profileFor(hz);
    const float semi = p ? 12.0f * log2f(hz / p->f0) : 0.0f;
    printf("%4s%-2d %8.1f %10.1f %+9.1f 半音%s\n",
           kNames[m % 12], m / 12 - 1, hz, p ? p->f0 : 0.0f, semi,
           (hz < bankLo * 0.99f || hz > bankHi * 1.01f) ? "   <- 外推" : "");

    gSynth.noteOn((float)m, 100.0f / 127.0f, 0.5f);
    for (int b = 0; b < blocksPerNote; b++) {
      // score.cpp's dur == NOTE_TICKS, so the release instant is this slot's end.
      // Send noteOff one block early so it doesn't collide with the next noteOn.
      if (b == blocksPerNote - 1) gSynth.noteOff((float)m);
      gSynth.update();
      for (int k = 0; k < TC_BLOCK; k++) {
        pcm.push_back(sim_outL[k]);
        pcm.push_back(sim_outR[k]);
      }
    }
  }
  // Let the last note's release run out
  gSynth.allNotesOff();
  for (int b = 0; b < (int)(1.5f / TC_BLOCK_SEC); b++) {
    gSynth.update();
    for (int k = 0; k < TC_BLOCK; k++) {
      pcm.push_back(sim_outL[k]);
      pcm.push_back(sim_outR[k]);
    }
  }

  // ---- Write the file -----------------------------------------------------
  FILE *f = fopen(outPath, "wb");
  if (!f) { printf("無法寫入 %s\n", outPath); return 1; }
  const uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
  auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f); w32(36 + dataBytes);
  fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f); w32(16);
  w16(1); w16(2); w32((uint32_t)TC_SAMPLE_RATE);
  w32((uint32_t)(TC_SAMPLE_RATE * 2 * 2)); w16(4); w16(16);
  fwrite("data", 1, 4, f); w32(dataBytes);
  fwrite(pcm.data(), 2, pcm.size(), f);
  fclose(f);
  printf("\n已寫出 %s（%.1f 秒）\n", outPath, pcm.size() / 2.0f / TC_SAMPLE_RATE);
  return 0;
}

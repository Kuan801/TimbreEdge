// ============================================================================
//  midi_test.cpp  -  verify the MIDI keyboard logic on the desktop
//
//  Usage:  make midi_test && ./midi_test material1.WAV [material2.WAV ...]
//
//  MidiInput::feed() is the platform-independent entry point, so the sustain
//  pedal, velocity curve, pitch bend and polyphonic voice allocation can all be
//  tested without plugging in a real keyboard. The output midi_test.wav can be
//  listened to directly.
// ============================================================================
#include <Arduino.h>
#include <Audio.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../config.h"
#include "../../profile.h"
#include "../../analyzer.h"
#include "../../timbre_model.h"
#include "../../additive_synth.h"
#include "../../midi_in.h"
#include "../../wav_io.h"

static ProfileBank        gBank;
static TimbreModel        gModel;
static AudioSynthAdditive gSynth;

static int   gFail = 0;
static void check(const char *what, bool ok, const char *detail = "") {
  printf("  %-46s %s %s\n", what, ok ? "通過" : "**失敗**", detail);
  if (!ok) gFail++;
}

// Run the synth for ms milliseconds and collect the samples into out.
// The simulator's transmit() writes into the globals sim_outL / sim_outR.
static void run(float ms, std::vector<int16_t> *out) {
  const int blocks = (int)(ms / 1000.0f / TC_BLOCK_SEC + 0.5f);
  for (int i = 0; i < blocks; i++) {
    gSynth.update();
    if (out)
      for (int k = 0; k < TC_BLOCK; k++) {
        out->push_back(sim_outL[k]);
        out->push_back(sim_outR[k]);
      }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { printf("用法：./midi_test 素材1.WAV [...]\n"); return 1; }

  // ---- Build the timbre bank ----------------------------------------------
  for (int i = 1; i < argc; i++) {
    InstrumentProfile p;
    if (analyzeWavFile(argv[i], p, nullptr, nullptr)) { p.valid = true; gBank.add(p); }
  }
  if (gBank.n == 0) { printf("沒有可用的素材\n"); return 1; }
  gModel.setBank(&gBank);
  gModel.setProfile(&gBank.p[0]);
  gSynth.setModel(&gModel);
  gSynth.setMasterGain(0.18f);
  gSynth.setVibrato(50.0f, 4.8f);
  gMidi.begin(&gSynth);

  printf("\n=== MIDI 鍵盤邏輯測試（%d 個 profile）===\n\n", gBank.n);
  std::vector<int16_t> pcm;

  // ---- 1) Single note -----------------------------------------------------
  printf("1) 基本發聲\n");
  gMidi.feed(0x90, 60, 100);              // C4
  run(50, &pcm);
  check("按下 C4 之後有聲部在發聲", gSynth.activeVoices() == 1);
  check("heldNotes() 追蹤正確", gMidi.heldNotes() == 1);
  gMidi.feed(0x80, 60, 0);
  // The piano profile measures R=0.2, so the release really needs about 0.58 s to reach zero -- wait longer
  run(900, &pcm);
  check("放開後聲部收乾淨", gSynth.activeVoices() == 0);

  // ---- 2) NoteOn with velocity 0 == NoteOff -------------------------------
  printf("\n2) 力度 0 的 NoteOn（很多鍵盤用這個代替 NoteOff）\n");
  gMidi.feed(0x90, 62, 90);
  run(50, &pcm);
  const bool on = gSynth.activeVoices() == 1;
  gMidi.feed(0x90, 62, 0);                // velocity 0
  run(900, &pcm);
  check("被正確當成 NoteOff", on && gSynth.activeVoices() == 0);

  // ---- 3) Polyphony -------------------------------------------------------
  printf("\n3) 複音（C 大三和弦 + 超出聲部數）\n");
  gMidi.feed(0x90, 60, 100);
  gMidi.feed(0x90, 64, 100);
  gMidi.feed(0x90, 67, 100);
  run(300, &pcm);
  check("三和弦同時發出 3 個聲部", gSynth.activeVoices() == 3);
  for (int n = 0; n < TC_N_VOICES + 3; n++) gMidi.feed(0x90, 48 + n, 100);
  run(100, &pcm);
  char msg[64];
  snprintf(msg, sizeof(msg), "(實際 %d)", gSynth.activeVoices());
  check("超額按鍵時不超過 TC_N_VOICES", gSynth.activeVoices() <= TC_N_VOICES, msg);
  gMidi.feed(0xB0, TC_MIDI_CC_ALLOFF, 0);
  run(900, &pcm);
  check("All Notes Off 全部靜音", gSynth.activeVoices() == 0);

  // ---- 4) Sustain pedal ---------------------------------------------------
  printf("\n4) 延音踏板（Keystation 的 SUST 鍵）\n");
  gMidi.feed(0xB0, TC_MIDI_CC_SUSTAIN, 127);   // pedal down
  gMidi.feed(0x90, 60, 100);
  run(80, &pcm);
  gMidi.feed(0x80, 60, 0);                     // release the key
  run(300, &pcm);
  check("踩著踏板時放開琴鍵，聲音續留", gSynth.activeVoices() == 1);
  gMidi.feed(0xB0, TC_MIDI_CC_SUSTAIN, 0);     // release the pedal
  run(900, &pcm);
  check("放開踏板後才真的收音", gSynth.activeVoices() == 0);

  // Re-pressing the same note while the pedal is held must not be cut off when the pedal is released
  gMidi.feed(0xB0, TC_MIDI_CC_SUSTAIN, 127);
  gMidi.feed(0x90, 62, 100);
  run(60, &pcm);
  gMidi.feed(0x80, 62, 0);
  gMidi.feed(0x90, 62, 100);                   // pressed again
  run(60, &pcm);
  gMidi.feed(0xB0, TC_MIDI_CC_SUSTAIN, 0);
  run(60, &pcm);
  check("踏板期間重按的音不會被誤切", gSynth.activeVoices() == 1);
  gMidi.panic();
  run(400, &pcm);

  // ---- 5) Velocity --------------------------------------------------------
  printf("\n5) 力度對應\n");
  auto peakOf = [&](uint8_t vel) {
    std::vector<int16_t> b;
    gMidi.feed(0x90, 60, vel);
    run(250, &b);
    gMidi.panic();
    run(400, nullptr);
    int mx = 0;
    for (int16_t v : b) if (abs(v) > mx) mx = abs(v);
    return mx;
  };
  const int pSoft = peakOf(20), pMid = peakOf(70), pHard = peakOf(127);
  snprintf(msg, sizeof(msg), "(20->%d  70->%d  127->%d)", pSoft, pMid, pHard);
  check("力度越大音量越大", pSoft < pMid && pMid < pHard, msg);
  check("最弱奏仍聽得見（沒有被門檻吃掉）", pSoft > 200);
  check("最強奏沒有削波", pHard < 32700);

  // ---- 6) Pitch bend ------------------------------------------------------
  printf("\n6) 彎音輪\n");
  auto freqOf = [&](int bend14) {
    std::vector<int16_t> b;
    gMidi.feed(0xE0, bend14 & 0x7F, (bend14 >> 7) & 0x7F);
    gMidi.feed(0x90, 60, 110);
    run(900, &b);
    gMidi.panic();
    run(900, nullptr);
    // Sweep a DFT over 200~330 Hz directly to find the fundamental peak.
    // Autocorrelation will not do here: the window has to be long for frequency
    // resolution, and the piano's decay depresses the correlation at large lags,
    // so it easily picks the wrong peak.
    const int N = 32768, off = (int)(0.15f * TC_SAMPLE_RATE) * 2;
    if ((int)b.size() < off + N * 2) return 0.0f;
    std::vector<float> x(N);
    for (int i = 0; i < N; i++)
      x[i] = b[off + i * 2] / 32768.0f * (0.5f - 0.5f * cosf(2 * M_PI * i / (N - 1)));
    float best = 0, bf = 0;
    for (float f = 200.0f; f <= 330.0f; f += 0.05f) {
      float re = 0, im = 0, ph = 0, dp = 2 * M_PI * f / TC_SAMPLE_RATE;
      for (int i = 0; i < N; i++) { re += x[i] * cosf(ph); im -= x[i] * sinf(ph); ph += dp; }
      float m = re * re + im * im;
      if (m > best) { best = m; bf = f; }
    }
    return bf;
  };
  const float f0 = freqOf(8192);                       // centre
  const float fUp = freqOf(16383);                     // pushed all the way up
  const float fDn = freqOf(0);                         // pulled all the way down
  const float cUp = 1200.0f * log2f(fUp / f0);
  const float cDn = 1200.0f * log2f(fDn / f0);
  snprintf(msg, sizeof(msg), "(%.1f Hz -> %+.0f / %+.0f cents)", f0, cUp, cDn);
  check("彎音上下各約 200 cents（+-2 半音）",
        fabsf(cUp - 200) < 25 && fabsf(cDn + 200) < 25, msg);
  gMidi.feed(0xE0, 0, 64);                             // back to centre
  gMidi.panic();

  // ---- 7) Output file -----------------------------------------------------
  printf("\n7) 產生試聽檔\n");
  const int seq[][2] = {{60,0},{64,150},{67,300},{72,450},{71,700},{69,850},{67,1000},{60,1300}};
  const size_t before = pcm.size();
  gMidi.feed(0xB0, TC_MIDI_CC_SUSTAIN, 127);
  float t = 0;
  for (auto &e : seq) {
    while (t < e[1]) { run(TC_BLOCK_SEC * 1000.0f, &pcm); t += TC_BLOCK_SEC * 1000.0f; }
    gMidi.feed(0x90, (uint8_t)e[0], 100);
  }
  run(1500, &pcm);
  gMidi.feed(0xB0, TC_MIDI_CC_SUSTAIN, 0);
  run(1200, &pcm);
  check("試聽段有產生音訊", pcm.size() > before + 44100);

  WavWriter w;
  if (w.open("midi_test.wav", TC_SAMPLE_RATE, 2)) {
    w.writeSamples(pcm.data(), pcm.size());
    w.close();
    printf("  -> midi_test.wav（%.1f 秒）\n", pcm.size() / 2.0f / TC_SAMPLE_RATE);
  }

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

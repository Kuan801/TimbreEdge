// ============================================================================
//  tools/sim/sim_main.cpp
//
//  Desktop simulator: renders the canon to a WAV file using exactly the same
//  analyzer / timbre_model / additive_synth / score / player sources that run on
//  the Teensy. Listening on a computer before flashing is far quicker than
//  swapping the SD card back and forth.
//
//  Build and run:
//     cd TimbreClone/tools/sim && make && ./sim <source.wav> [MODEL.BIN] [out.wav]
// ============================================================================

#include "Arduino.h"
#include "Audio.h"
#include "SD.h"

#include "../../config.h"
#include "../../profile.h"
#include "../../analyzer.h"
#include "../../timbre_model.h"
#include "../../additive_synth.h"
#include "../../player.h"
#include "../../score.h"
#include "../../trainer.h"

#include <string>
#include <vector>

// ----------------------------------------------------------- WAV output ----
static void writeWav(const char *path, const std::vector<int16_t> &pcm, int ch, int sr) {
  FILE *f = fopen(path, "wb");
  if (!f) { printf("無法寫入 %s\n", path); return; }
  uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
  auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f); w32(36 + dataBytes);
  fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f); w32(16);
  w16(1); w16((uint16_t)ch); w32((uint32_t)sr);
  w32((uint32_t)(sr * ch * 2)); w16((uint16_t)(ch * 2)); w16(16);
  fwrite("data", 1, 4, f); w32(dataBytes);
  fwrite(pcm.data(), 2, pcm.size(), f);
  fclose(f);
  printf("已寫出 %s  (%.2f 秒, %d ch)\n", path, pcm.size() / (float)(sr * ch), ch);
}

// ---------------------------------------------------------------------------
//  train mode: run exactly the same training code as on the Teensy
//     ./sim train MODEL.BIN note1.wav note2.wav ...
//  Used to confirm before flashing how far on-device training will converge,
//  much faster than waiting on the serial port.
// ---------------------------------------------------------------------------
static MlpWeights gOutW;
static ProfileBank gBank;

static int runTrain(int argc, char **argv) {
  if (argc < 4) {
    printf("用法：./sim train MODEL.BIN note1.wav [note2.wav ...]\n");
    return 1;
  }
  const char *outPath = argv[2];

  TrainSet ts;
  ts.clear();
  InstrumentProfile prof;

  for (int i = 3; i < argc; i++) {
    printf("--- 加入 %s ---\n", argv[i]);
    int before = ts.size();
    if (!analyzeWavFile(argv[i], prof, nullptr, &ts)) {
      printf("  ! 分析失敗，跳過\n");
      continue;
    }
    printf("  加入 %d 筆樣本\n", ts.size() - before);
  }
  ts.summary();

  if (!trainMlp(ts, gOutW, TC_TRAIN_EPOCHS, TC_TRAIN_LR, 12345)) return 1;

  FILE *f = fopen(outPath, "wb");
  if (!f) { printf("無法寫入 %s\n", outPath); return 1; }
  fwrite(&gOutW, 1, sizeof(MlpWeights), f);
  fclose(f);
  printf("已寫出 %s (%u bytes)\n", outPath, (unsigned)sizeof(MlpWeights));
  return 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "train") == 0) return runTrain(argc, argv);

  // Usage: ./sim out.wav model.bin note1.wav [note2.wav ...]
  //   Every source goes into the timbre bank and each note picks the closest one.

  // The command line gives real paths; do not prepend the simulated SD root
  { extern std::string sim_sd_root; sim_sd_root = ""; }

  const char *out   = argv[1];
  const char *model = (argc > 2 && argv[2][0]) ? argv[2] : nullptr;

  InstrumentProfile prof;
  TimbreModel       modelObj;
  AudioSynthAdditive synth;
  Player            player;

  printf("=== 1) 分析素材 ===\n");
  gBank.clear();
  for (int i = 3; i < argc; i++) {
    if (!analyzeWavFile(argv[i], prof, nullptr)) { printf("  ! %s 分析失敗\n", argv[i]); continue; }
    gBank.add(prof);
  }
  if (gBank.n == 0) { printf("沒有可用的素材\n"); return 1; }
  gBank.summary();
  modelObj.setProfile(&gBank.p[0]);
  modelObj.setBank(&gBank);

  printf("\n=== 2) 載入模型 ===\n");
  if (model) modelObj.loadWeights(model);
  else       printf("(未指定 MODEL.BIN，使用關鍵影格內插模式)\n");

  printf("\n=== 3) 演奏半音階 ===\n");

  // Same range rule as the firmware: the range the bank actually covers, plus one
  // octave.
  //
  // This used to be missing, so the simulator always played a hard-coded C3~B4.
  // With C4~B4 source material the simulator would transpose every note down an
  // octave -- something the firmware never does -- and taking the simulator's
  // result as representative of the hardware then distorts it, in the pessimistic
  // direction.
  {
    int lo = 127, hi = 0;
    for (int i = 0; i < gBank.n; i++) {
      const int m = (int)lroundf(69.0f + 12.0f * log2f(gBank.p[i].f0 / 440.0f));
      if (m < lo) lo = m;
      if (m > hi) hi = m;
    }
    if (lo <= hi) {
      scoreSetScaleRange(lo, hi + 12);
      player.load();                       // Changing the range means the score has to be regenerated
    }
  }

  synth.setModel(&modelObj);
  synth.setMasterGain(0.18f);
  synth.setVibrato(50.0f, 4.8f);   // Cap and default rate; the actual depth/frequency come from the measured profile
  player.begin(&synth);
  player.start(TC_BPM);

  std::vector<int16_t> pcm;
  const uint32_t blockUs = (uint32_t)(AUDIO_BLOCK_SAMPLES * 1e6 / TC_SAMPLE_RATE);
  int   blocks = 0;
  int   tailBlocks = 0;
  float peak = 0.0f;
  double rmsAcc = 0.0;

  while (blocks < 60 * 344) {                 // 60 s cap, as a safety net
    sim_micros += blockUs;
    player.service();

    memset(sim_outL, 0, sizeof(sim_outL));
    memset(sim_outR, 0, sizeof(sim_outR));
    synth.update();

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      pcm.push_back(sim_outL[i]);
      pcm.push_back(sim_outR[i]);
      float a = fabsf(sim_outL[i] / 32768.0f);
      if (a > peak) peak = a;
      rmsAcc += (double)a * a;
    }
    blocks++;

    if (!player.playing()) {
      if (synth.activeVoices() == 0 && ++tailBlocks > 172) break;   // Wait another 0.5 s after the tail has rung out
    }
  }

  double rms = sqrt(rmsAcc / (blocks * AUDIO_BLOCK_SAMPLES));
  printf("\n=== 4) 結果 ===\n");
  printf("  總長 %.2f 秒 / %d blocks\n", blocks * AUDIO_BLOCK_SAMPLES / TC_SAMPLE_RATE, blocks);
  printf("  峰值 %.3f   RMS %.4f (%.1f dBFS)\n", peak, rms, 20 * log10(rms + 1e-12));
  if (peak > 0.999f) printf("  ! 警告：削波，請調低 setMasterGain\n");
  if (peak < 0.05f)  printf("  ! 警告：太小聲\n");

  writeWav(out, pcm, 2, (int)TC_SAMPLE_RATE);
  return 0;
}

// ============================================================================
//  tools/sim/canon.cpp  -  用現成的 BANK.BIN 在桌機重現「卡農」那條路徑
//
//  跟 sim 的差別：不重新分析素材，直接吃機器上存下來的 BANK.BIN。
//  目的是把「實機錄到的 CANON.WAV」跟「同一組參數在桌機算出來的」對齊，
//  才分得出問題出在參數、在合成器、還是在實機的音訊鏈（reverb / codec）。
//
//  用法：./canon BANK.BIN out.wav [MODEL.BIN] [ablate]
//    ablate 是一串字母，用來把某個參數歸零再算一次，逐項排除：
//      i = inharmonicity   a = attackNoise   s = shimmerDepth
//      n = noiseGain       v = vibrato       h = harmOnset
// ============================================================================
#include "Arduino.h"
#include "Audio.h"
#include "SD.h"
#include "../../config.h"
#include "../../profile.h"
#include "../../timbre_model.h"
#include "../../additive_synth.h"
#include "../../player.h"
#include "../../score.h"
#include <string>
#include <vector>

static void writeWav(const char *path, const std::vector<int16_t> &pcm, int ch, int sr) {
  FILE *f = fopen(path, "wb");
  if (!f) { printf("無法寫入 %s\n", path); return; }
  uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
  auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF",1,4,f); w32(36+dataBytes); fwrite("WAVE",1,4,f);
  fwrite("fmt ",1,4,f); w32(16); w16(1); w16(ch); w32(sr);
  w32(sr*ch*2); w16(ch*2); w16(16);
  fwrite("data",1,4,f); w32(dataBytes);
  fwrite(pcm.data(),2,pcm.size(),f); fclose(f);
  printf("已寫出 %s (%.2f 秒)\n", path, pcm.size()/(float)(sr*ch));
}

static ProfileBank gBank;

int main(int argc, char **argv) {
  if (argc < 3) { printf("用法：./canon BANK.BIN out.wav [MODEL.BIN] [ablate]\n"); return 1; }
  const char *bankPath = argv[1], *outPath = argv[2];
  const char *model = (argc > 3 && argv[3][0] && strcmp(argv[3],"-")) ? argv[3] : nullptr;
  std::string ab = (argc > 4) ? argv[4] : "";

  sim_sd_root = ".";
  if (!bankLoad(gBank, bankPath)) { printf("讀不到 %s\n", bankPath); return 1; }

  // 逐項歸零：一次只動一個參數，才知道是哪一項在發出聲音
  for (int i = 0; i < gBank.n; i++) {
    InstrumentProfile &p = gBank.p[i];
    if (ab.find('i') != std::string::npos) p.inharmonicity = 0.0f;
    if (ab.find('a') != std::string::npos) p.attackNoise   = p.noiseGain;
    if (ab.find('s') != std::string::npos) p.shimmerDepth  = 0.0f;
    if (ab.find('n') != std::string::npos) p.noiseGain     = 0.0f;
    if (ab.find('v') != std::string::npos) { p.vibratoCents = 0.0f; p.vibratoHz = 0.0f; }
    if (ab.find('h') != std::string::npos) for (int k = 0; k < TC_N_HARM; k++) p.harmOnset[k] = 0.0f;
    // r = 「修過的」：B 歸零（提琴本來就該是 0）、shimmer 拉回合理區間、
    //     起音噪聲設上限。目的是聽聽看這三項修好之後差多少。
    if (ab.find('r') != std::string::npos) {
      p.inharmonicity = 0.0f;
      if (p.shimmerDepth <= 0.001f || p.shimmerDepth >= 0.199f) p.shimmerDepth = 0.12f;
      if (p.attackNoise > 0.45f) p.attackNoise = 0.45f;
      if (p.attackNoise < 0.10f) p.attackNoise = 0.25f;
    }
  }
  if (!ab.empty()) printf("[ABLATE] 已歸零：%s\n", ab.c_str());

  // 韌體在演奏前會呼叫 applyScaleRangeFromBank()，把「音域」設成音色庫的
  // 範圍再往上一個八度 —— pickOctaveShift() 用的就是這組全域值。
  // 少了這一步，八度安置會用預設的 C3~B4，算出來的移調量跟實機不一樣。
  {
    int lo = 127, hi = 0;
    for (int i = 0; i < gBank.n; i++) {
      const int m = (int)lroundf(69.0f + 12.0f * log2f(gBank.p[i].f0 / 440.0f));
      if (m < 0 || m > 127) continue;
      if (m < lo) lo = m;
      if (m > hi) hi = m;
    }
    if (lo <= hi) { scoreSetScaleRange(lo, hi + 12); printf("[SIM] 音域設為 %d~%d\n", lo, hi + 12); }
  }

  TimbreModel modelObj;
  AudioSynthAdditive synth;
  Player player;
  modelObj.setProfile(&gBank.p[0]);
  modelObj.setBank(&gBank);
  if (model) modelObj.loadWeights(model);

  scoreSetMode(TC_SCORE_CANON);
  player.load();
  synth.setModel(&modelObj);
  synth.setMasterGain(0.18f);
  synth.setVibrato(50.0f, 4.8f);
  player.begin(&synth);
  player.start(TC_BPM);

  std::vector<int16_t> pcm;
  const uint32_t blockUs = (uint32_t)(AUDIO_BLOCK_SAMPLES * 1e6 / TC_SAMPLE_RATE);
  int blocks = 0, tail = 0;
  while (blocks < 90 * 344) {
    sim_micros += blockUs;
    player.service();
    memset(sim_outL, 0, sizeof(sim_outL));
    memset(sim_outR, 0, sizeof(sim_outR));
    synth.update();
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { pcm.push_back(sim_outL[i]); pcm.push_back(sim_outR[i]); }
    blocks++;
    if (!player.playing() && synth.activeVoices() == 0 && ++tail > 172) break;
  }
  writeWav(outPath, pcm, 2, (int)TC_SAMPLE_RATE);
  return 0;
}

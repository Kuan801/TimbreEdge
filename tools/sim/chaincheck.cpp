#include <Arduino.h>
#include <Audio.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

#include "../../config.h"
#include "../../profile.h"
#include "../../analyzer.h"

static const float kBandLo[] = { 60, 125, 250, 500, 1000, 2000, 4000, 8000 };
static const int   kNBand    = (int)(sizeof(kBandLo) / sizeof(kBandLo[0]));

struct Res { float db[8]; bool has[8]; };

static bool harmDb(const char *path, float *outDb, float *outF0, int nh = 24) {
  InstrumentProfile p;
  if (!analyzeWavFile(path, p, nullptr, nullptr)) return false;
  *outF0 = p.f0;

  const float *kf = p.keyframe[TC_N_KEYFRAME / 2];
  float mx = 0.0f;
  for (int h = 0; h < nh && h < TC_N_HARM; h++) if (kf[h] > mx) mx = kf[h];
  if (mx <= 0.0f) return false;
  for (int h = 0; h < nh && h < TC_N_HARM; h++)
    outDb[h] = 20.0f * log10f(kf[h] / mx + 1e-9f);
  return true;
}

int main(int argc, char **argv) {
  extern std::string sim_sd_root;
  sim_sd_root = "";

  if (argc < 3 || (argc - 1) % 2 != 0) {
    printf("用法：./chaincheck 參考檔.wav 錄音檔.wav [參考檔2 錄音檔2 ...]\n");
    printf("      兩兩一組，每組必須是同一個音。\n");
    return 1;
  }

  std::vector<float> acc[8];
  int pairs = 0;

  for (int i = 1; i + 1 < argc; i += 2) {
    float refDb[24], recDb[24], f0r = 0, f0c = 0;
    if (!harmDb(argv[i], refDb, &f0r)) { printf("  ! 讀不到 %s\n", argv[i]); continue; }
    if (!harmDb(argv[i + 1], recDb, &f0c)) { printf("  ! 讀不到 %s\n", argv[i + 1]); continue; }

    const float cents = 1200.0f * log2f(f0c / f0r);
    if (fabsf(cents) > 60.0f) {
      printf("  ! 這一組的音高差 %.0f cents，不是同一個音，跳過\n", cents);
      continue;
    }
    pairs++;

    for (int h = 0; h < 24; h++) {
      const float hz = f0r * (h + 1);
      for (int b = 0; b < kNBand; b++) {
        const float hi = (b + 1 < kNBand) ? kBandLo[b + 1] : 16000.0f;
        if (hz >= kBandLo[b] && hz < hi) { acc[b].push_back(recDb[h] - refDb[h]); break; }
      }
    }
  }

  if (!pairs) { printf("沒有可用的配對\n"); return 1; }

  printf("\n=== 錄音鏈頻率響應（%d 組配對）===\n\n", pairs);

  float base = 0.0f;
  bool haveBase = false;
  Res r{};
  for (int b = 0; b < kNBand; b++) {
    r.has[b] = acc[b].size() >= 2;
    if (!r.has[b]) continue;
    std::sort(acc[b].begin(), acc[b].end());
    r.db[b] = acc[b][acc[b].size() / 2];
    if (!haveBase) { base = r.db[b]; haveBase = true; }
  }

  printf("  %-14s %10s %8s   %s\n", "頻段 (Hz)", "相對增益", "樣本數", "");
  float worst = 0.0f;
  for (int b = 0; b < kNBand; b++) {
    if (!r.has[b]) { printf("  %5.0f - %-6.0f %10s\n",
                            kBandLo[b],
                            (b + 1 < kNBand) ? kBandLo[b + 1] : 16000.0f, "—"); continue; }
    const float v = r.db[b] - base;
    if (fabsf(v) > fabsf(worst)) worst = v;

    char bar[42] = {0};
    int n = (int)(fabsf(v) / 3.0f);
    if (n > 20) n = 20;
    for (int k = 0; k < n; k++) bar[k] = '#';
    printf("  %5.0f - %-6.0f %+9.1f %8zu   %s\n",
           kBandLo[b], (b + 1 < kNBand) ? kBandLo[b + 1] : 16000.0f,
           v, acc[b].size(), bar);
  }

  printf("\n-- 判讀 --\n");
  if (fabsf(worst) < 6.0f) {
    printf("  最大偏差 %.1f dB —— 錄音鏈相當平坦，可以放心採樣。\n", worst);
  } else if (fabsf(worst) < 15.0f) {
    printf("  最大偏差 %.1f dB —— 有明顯著色，但還在可用範圍。\n", worst);
    printf("  想更好的話可以調整麥克風距離與角度，再跑一次看有沒有改善。\n");
  } else {
    printf("  最大偏差 %.1f dB —— 錄音鏈嚴重不平坦，音色分析會被污染。\n", worst);
    printf("  這個量級通常不是麥克風擺位造成的，優先檢查：\n");
    printf("   1. 訊號源是不是小喇叭（筆電/手機喇叭 300 Hz 以下幾乎沒有輸出，\n");
    printf("      而且 2~4 kHz 會隆起 —— 這條曲線的形狀就長這樣）\n");
    printf("   2. 是不是離樂器太遠，收到的主要是房間反射而不是直達音\n");
    printf("   3. 麥克風本身的低頻響應（小型駐極體麥克風的低頻通常不好）\n");
    printf("  在合成端補償沒有意義 —— 被吃掉的基頻不會因為放大就回來，\n");
    printf("  只會把噪聲一起放大。\n");
  }
  return 0;
}

// ============================================================================
//  chaincheck.cpp  -  量錄音鏈的頻率響應
//
//  用法：  ./chaincheck 參考檔.wav 錄音檔.wav [參考檔2 錄音檔2 ...]
//
//  兩個檔案必須是「同一個音」：參考檔是乾淨的來源，錄音檔是經過
//  你的麥克風、線材、SGTL5000 之後錄下來的同一個音。
//
//  --- 為什麼需要這支工具 -----------------------------------------------------
//
//  實測發現麥克風錄的鋼琴 C4，第 4 根諧波比基頻強 20 dB；
//  同一個音的參考素材卻是基頻比第 4 根強 22 dB —— 兩者差了 42 dB。
//  也就是錄到的聲音「基頻幾乎不存在」。
//
//  合成器只是忠實重現它拿到的素材，所以這個問題在合成端修不了。
//  但它也不是「聽起來怪怪的」這種模糊描述 —— 它是一條可以量出來的曲線。
//  有了數字，就能一邊調麥克風位置一邊看有沒有改善，而不是憑感覺試。
//
//  --- 怎麼用 ----------------------------------------------------------------
//
//  1. 挑一個乾淨的參考音檔（例如 Piano.mf.C4.wav）
//  2. 用你實際的錄音方式錄同一個音
//  3. 跑這支工具，看各頻段的相對增益
//
//  理想結果是每個頻段都接近 0 dB（表示錄音鏈是平坦的）。
//  低頻負很多 = 低頻被吃掉了；中高頻正很多 = 有共振或訊號源本身沒有低頻。
//
//  給多組（不同音高）會一起統計，因為單一個音的結果會被那個音自己的
//  諧波結構影響 —— 多個音高平均之後才看得出「錄音鏈」本身的特性。
// ============================================================================
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

// 以八度為單位的頻段。低頻那幾段最能看出問題。
static const float kBandLo[] = { 60, 125, 250, 500, 1000, 2000, 4000, 8000 };
static const int   kNBand    = (int)(sizeof(kBandLo) / sizeof(kBandLo[0]));

struct Res { float db[8]; bool has[8]; };

// 量一個檔案的諧波振幅（dB，以自身最大值為 0）
static bool harmDb(const char *path, float *outDb, float *outF0, int nh = 24) {
  InstrumentProfile p;
  if (!analyzeWavFile(path, p, nullptr, nullptr)) return false;
  *outF0 = p.f0;
  // 用持續段的關鍵影格當代表（避開起音瞬態）
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

    // 音高差太多就不是同一個音，比了沒有意義
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

  // 以最低的有效頻段當 0 dB 基準，看的是「傾斜」而不是絕對增益
  float base = 0.0f;
  bool haveBase = false;
  Res r{};
  for (int b = 0; b < kNBand; b++) {
    r.has[b] = acc[b].size() >= 2;
    if (!r.has[b]) continue;
    std::sort(acc[b].begin(), acc[b].end());
    r.db[b] = acc[b][acc[b].size() / 2];          // 中位數，比平均穩健
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
    // 簡單的長條圖，一格 3 dB
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

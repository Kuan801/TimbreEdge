// ============================================================================
//  recscan  -  把一堆 WAV 丟進去，看機器上會給出什麼判定
//
//  用途：改了 rec_check.cpp 的門檻之後，拿真實素材驗一遍再燒錄。
//  reccheck_test 驗的是規則本身（給定數字會不會判對），這支驗的是
//  「分析器量出來的數字」跟「規則」接起來的結果 —— 兩件不同的事。
//
//  用法：
//    ./recscan 乾淨素材/*.wav          <- 應該全部 ok
//    ./recscan 麥克風錄的/*.WAV        <- 錄壞的應該被抓出來
// ============================================================================
#include "../../analyzer.h"
#include "../../profile.h"
#include "../../rec_check.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

static const char *kName[12] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("用法：./recscan a.wav [b.wav ...]\n");
    return 1;
  }

  // 桌機的 SD 模擬預設會在路徑前面接一個根目錄（模擬 SD 卡的 "/"）。
  // 這支工具吃的是命令列給的真實路徑，所以要把它清掉。
  extern std::string sim_sd_root;
  sim_sd_root = "";

  printf("%-28s %-6s %5s %6s %5s %3s %5s %5s  %s\n",
         "檔案", "判定", "音高", "峰值", "SNR", "起音", "音長", "衰減", "說明");

  int nOk = 0, nWarn = 0, nBad = 0;

  for (int i = 1; i < argc; i++) {
    InstrumentProfile prof;
    const bool ok = analyzeWavFile(argv[i], prof, nullptr, nullptr);

    RecCheck rc;
    rc.analysisOk  = ok;
    rc.peak        = analyzerLastPeak();
    rc.clipRatio   = analyzerLastClipRatio();
    rc.noiseFloor  = analyzerLastNoiseFloor();
    rc.onsets      = analyzerLastOnsetCount();
    rc.noteDur     = ok ? prof.noteDur : 0.0f;
    rc.f0          = ok ? prof.f0      : 0.0f;
    rc.decayPerSec = ok ? prof.sustainDecayPerSec : 0.0f;

    char reason[26], fix[26];
    const RecVerdict v = recCheckEval(rc, reason, sizeof(reason), fix, sizeof(fix));
    if      (v == REC_OK)   nOk++;
    else if (v == REC_WARN) nWarn++;
    else                    nBad++;

    // 只留檔名，路徑太長會把表格撐爛
    const char *base = strrchr(argv[i], '/');
    base = base ? base + 1 : argv[i];

    char note[10] = "-";
    if (ok && rc.f0 > 0) {
      const int m = (int)lroundf(69.0f + 12.0f * log2f(rc.f0 / 440.0f));
      snprintf(note, sizeof(note), "%s%d", kName[((m % 12) + 12) % 12], m / 12 - 1);
    }

    char snr[8];
    if (recCheckSnrKnown(rc.noiseFloor))
      snprintf(snr, sizeof(snr), "%.0f", recCheckSnrDb(rc.noiseFloor));
    else
      snprintf(snr, sizeof(snr), "--");      // 起音在第 0 格，沒有底噪可量

    printf("%-28.28s %-6s %5s %6.3f %5s %3d %5.2f %5.2f  %s / %s\n",
           base,
           v == REC_OK ? "ok" : (v == REC_WARN ? "warn" : "BAD"),
           note, rc.peak, snr, rc.onsets,
           rc.noteDur, rc.decayPerSec, reason, fix);
  }

  printf("\n合計：ok %d、warn %d、BAD %d\n", nOk, nWarn, nBad);
  return 0;
}

// ============================================================================
//  recscan  -  throw a pile of WAVs at it and see what verdict the device gives
//
//  Purpose: after changing a threshold in rec_check.cpp, run real material
//  through it once before flashing.
//  reccheck_test checks the rules themselves (given these numbers, is the
//  verdict right); this one checks what comes out when "the numbers the
//  analyzer measures" are wired to "the rules" -- two different things.
//
//  Usage:
//    ./recscan clean-material/*.wav    <- should all be ok
//    ./recscan mic-takes/*.WAV         <- the bad takes should get caught
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

  // The desktop SD emulation prepends a root directory to paths by default
  // (emulating the SD card's "/"). This tool takes real paths off the command
  // line, so clear it.
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

    // Basename only; long paths blow the table apart
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
      snprintf(snr, sizeof(snr), "--");      // Attack sits in bin 0, so there is no noise floor to measure

    printf("%-28.28s %-6s %5s %6.3f %5s %3d %5.2f %5.2f  %s / %s\n",
           base,
           v == REC_OK ? "ok" : (v == REC_WARN ? "warn" : "BAD"),
           note, rc.peak, snr, rc.onsets,
           rc.noteDur, rc.decayPerSec, reason, fix);
  }

  printf("\n合計：ok %d、warn %d、BAD %d\n", nOk, nWarn, nBad);
  return 0;
}

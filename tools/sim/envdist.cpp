// ============================================================================
//  envdist.cpp  -  measure "how far apart two timbres' spectral envelopes are"
//
//  Usage:  ./envdist A.WAV B.WAV [C.WAV ...]
//
//  Why it is needed: ProfileBank::add() has to warn when "this timbre is very
//  different from what the bank already holds" (usually that means the
//  instrument was swapped without clearing the bank first). But the threshold
//  cannot be guessed -- too low and it cries wolf all day, too high and it may
//  as well not be there.
//
//  This tool prints the pairwise distance over all the material, so the
//  threshold rests on a measured distribution:
//    same instrument, different pitch  -> distance should be small (the envelope
//                                         is pitch-independent by design)
//    different instruments             -> distance should be large
//  Only if the two piles are far enough apart can a line be drawn between them.
// ============================================================================
#include <Arduino.h>
#include <Audio.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <map>

#include "../../config.h"
#include "../../profile.h"
#include "../../analyzer.h"

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("用法：./envdist A.WAV B.WAV [C.WAV ...]\n");
    return 1;
  }

  // The SD sim layer prepends a root directory to every path. Material is
  // usually scattered around, so set the root to "/" and absolute paths work
  // straight from the command line.
  extern std::string sim_sd_root;
  sim_sd_root = "";

  std::vector<InstrumentProfile> ps;
  std::vector<std::string>       names;

  for (int i = 1; i < argc; i++) {
    InstrumentProfile p;
    if (!analyzeWavFile(argv[i], p, nullptr, nullptr)) {
      printf("  ! %s 分析失敗\n", argv[i]);
      continue;
    }
    const char *b = strrchr(argv[i], '/');
    names.push_back(b ? b + 1 : argv[i]);
    ps.push_back(p);
    printf("  %-40s f0 %7.1f Hz\n", names.back().c_str(), p.f0);
  }
  const int n = (int)ps.size();
  if (n < 2) { printf("素材不足\n"); return 1; }

  // Instrument name = the part before the first '.' (Trumpet.vib.ff.C4 -> Trumpet)
  std::vector<std::string> inst;
  for (int i = 0; i < n; i++) {
    size_t d = names[i].find('.');
    inst.push_back(d == std::string::npos ? names[i] : names[i].substr(0, d));
  }

  std::vector<float> same, diff;
  std::map<std::string, std::vector<float>> crossPair;
  std::map<std::string, std::vector<float>> selfPair;
  // Print the farthest same-instrument pair -- a big number need not mean the
  // metric is bad, it can also mean bad material (a multi-note file slipped in,
  // a botched take). Without seeing which pair it is, you cannot tell them apart.
  std::vector<std::pair<float, std::string>> worstSame;

  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++) {
      const float d = profileTimbreDistance(ps[i], ps[j]);
      if (d <= 0.0f) continue;                 // too few overlapping bands, leave it out of the statistics
      if (inst[i] == inst[j]) {
        same.push_back(d);
        selfPair[inst[i]].push_back(d);
        worstSame.push_back({ d, names[i] + "  vs  " + names[j] });
      } else {
        diff.push_back(d);
        std::string a = inst[i], b = inst[j];
        if (a > b) std::swap(a, b);
        crossPair[a + " vs " + b].push_back(d);
      }
    }

  auto stat = [](std::vector<float> v, const char *label) {
    if (v.empty()) { printf("  %-24s （沒有配對）\n", label); return; }
    std::sort(v.begin(), v.end());
    const size_t m = v.size();
    printf("  %-24s n=%-5zu 最小 %6.2f  中位 %6.2f  95%% %6.2f  最大 %6.2f\n",
           label, m, v.front(), v[m / 2], v[(size_t)(m * 0.95)], v.back());
  };

  printf("\n=== 頻譜包絡距離分布（dB）===\n");
  stat(same, "同樂器、不同音高");
  stat(diff, "不同樂器");

  printf("\n--- 每種樂器自己內部 ---\n");
  for (auto &kv : selfPair) stat(kv.second, kv.first.c_str());

  printf("\n--- 各組樂器之間 ---\n");
  for (auto &kv : crossPair) stat(kv.second, kv.first.c_str());

  printf("\n--- 同樂器最遠的 8 對（挑素材問題用）---\n");
  std::sort(worstSame.begin(), worstSame.end(),
            [](const std::pair<float, std::string> &a,
               const std::pair<float, std::string> &b) { return a.first > b.first; });
  for (size_t i = 0; i < worstSame.size() && i < 8; i++)
    printf("  %6.2f  %s\n", worstSame[i].first, worstSame[i].second.c_str());

  // -------------------------------------------------------------------------
  //  Leave-one-out nearest neighbour
  //
  //  What insertion really has to ask is not "how far from the bank's average"
  //  but "how far from the closest set in the bank" -- synthesis picks the set
  //  with the nearest pitch anyway.
  //
  //  The average gets wrecked by the piano: its within-instrument distance
  //  reaches 15 dB, farther than "trumpet vs violin", so no absolute threshold
  //  can serve all four instruments at once. Nearest neighbour is different:
  //  however much a piano note varies, it always still has a neighbour it
  //  resembles.
  // -------------------------------------------------------------------------
  {
    std::vector<float> nnSame, nnCross;
    for (int i = 0; i < n; i++) {
      float bestSame = 1e9f, bestCross = 1e9f;
      for (int j = 0; j < n; j++) {
        if (i == j) continue;
        const float d = profileTimbreDistance(ps[i], ps[j]);
        if (d <= 0.0f) continue;
        if (inst[i] == inst[j]) { if (d < bestSame)  bestSame  = d; }
        else                    { if (d < bestCross) bestCross = d; }
      }
      if (bestSame  < 1e8f) nnSame.push_back(bestSame);
      if (bestCross < 1e8f) nnCross.push_back(bestCross);
    }
    printf("\n=== 留一法最近鄰距離（dB）===\n");
    stat(nnSame,  "跟同樂器最像的一組");
    stat(nnCross, "跟其他樂器最像的一組");

    printf("\n  門檻   同樂器誤報率   跨樂器偵測率  （最近鄰）\n");
    for (float th = 1.0f; th <= 3.01f; th += 0.1f) {
      size_t fp = 0, tp = 0;
      for (float d : nnSame)  if (d >= th) fp++;
      for (float d : nnCross) if (d >= th) tp++;
      printf("  %4.2f   %6.2f%% (%3zu)   %6.2f%% (%3zu)\n", th,
             nnSame.empty()  ? 0.0 : 100.0 * fp / nnSame.size(),  fp,
             nnCross.empty() ? 0.0 : 100.0 * tp / nnCross.size(), tp);
    }
  }

  // -------------------------------------------------------------------------
  //  Realistic scenario: the bank is full of instrument A, now add a note of B
  //
  //  This is the problem ProfileBank::add() actually faces. The "nearest over
  //  all other instruments" above is a pessimistic estimate -- in practice the
  //  bank only ever holds one instrument.
  // -------------------------------------------------------------------------
  {
    const float TH = 2.0f;
    std::vector<std::string> insts;
    for (auto &kv : selfPair) insts.push_back(kv.first);
    // selfPair only covers instruments that had a same-instrument pair; re-collect from inst to be safe
    insts.clear();
    for (int i = 0; i < n; i++)
      if (std::find(insts.begin(), insts.end(), inst[i]) == insts.end())
        insts.push_back(inst[i]);

    printf("\n=== 情境模擬：庫裡是「列」，新加入「欄」的一個音（門檻 %.1f）===\n", TH);
    printf("%-12s", "庫 \\ 新音");
    for (auto &c : insts) printf("%12.11s", c.c_str());
    printf("\n");

    for (auto &bank : insts) {
      printf("%-12.11s", bank.c_str());
      for (auto &nw : insts) {
        int hit = 0, tot = 0;
        for (int i = 0; i < n; i++) {
          if (inst[i] != nw) continue;
          float best = 1e9f;
          for (int j = 0; j < n; j++) {
            if (i == j || inst[j] != bank) continue;
            const float d = profileTimbreDistance(ps[i], ps[j]);
            if (d > 0.0f && d < best) best = d;
          }
          if (best > 1e8f) continue;
          tot++;
          if (best >= TH) hit++;
        }
        if (!tot) { printf("%12s", "-"); continue; }
        char buf[24];
        snprintf(buf, sizeof(buf), "%.0f%%", 100.0 * hit / tot);
        printf("%12s", buf);
      }
      printf("\n");
    }
    printf("  對角線 = 誤報率（同樂器不該叫），其餘 = 偵測率（換樂器該叫）\n");
  }

  // Suggested threshold: almost no false alarms within an instrument, catch as much cross-instrument as possible
  printf("\n=== 門檻掃描（用平均，供對照）===\n");
  printf("  門檻   同樂器誤報率   跨樂器偵測率\n");
  for (float th = 3.0f; th <= 9.01f; th += 0.5f) {
    size_t fp = 0, tp = 0;
    for (float d : same) if (d >= th) fp++;
    for (float d : diff) if (d >= th) tp++;
    printf("  %4.1f   %6.2f%% (%3zu)   %6.2f%% (%3zu)\n", th,
           same.empty() ? 0.0 : 100.0 * fp / same.size(), fp,
           diff.empty() ? 0.0 : 100.0 * tp / diff.size(), tp);
  }
  return 0;
}

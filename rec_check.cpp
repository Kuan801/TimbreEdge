#include "rec_check.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
//  門檻是怎麼訂出來的
//
//  這一節的數字全部來自實際錄壞的檔案，不是憑感覺訂的。素材是「用手機喇叭
//  播放鋼琴 C4 給麥克風收音」的兩份 REC.WAV，以及乾淨的 Iowa MIS 原始素材。
//
//    第一份 REC.WAV   峰值 1.000、203 個削波樣本（佔比 0.0023）
//                     動態範圍 2.9 dB（原始素材 24.5 dB）
//                     -> 波形被削平，量到的諧波幾乎都是削波產生的假諧波
//    第二份 REC.WAV   峰值 0.089、沒有削波，但觸發早了 1.7 秒
//                     觸發的是 30~190 Hz 的環境低頻，訊噪比只有 7 dB
//                     -> 2 秒的錄音裡只有最後 0.3 秒是真正的音
//
//  兩份的 peak 一個太大一個太小，靠單一個指標都抓不到「另一種」錯誤。
//  所以判定要同時看削波、音量、訊噪比、起音次數四件事。
//
//  一個誠實的限制：這些門檻抓得到「錄音鏈壞掉」這種量級的問題，
//  抓不到「錄得還行但不夠好」。它是體檢，不是評分。
// ============================================================================

#define CLIP_BAD        0.0005f   // 削波佔比。0.05% 已經是連續好幾段平頂了
#define PEAK_TOO_LOW    0.05f     // 低於這個，量化雜訊在諧波裡的比重開始失控
#define PEAK_LOW        0.15f     // 還能分析，但 SNR 已經不理想
#define PEAK_HOT        0.90f     // 還沒削波，但下一次大聲一點就會
#define NOISE_BAD       0.10f     // 訊噪比 20 dB。第二份 REC.WAV 是 0.45（7 dB）
#define NOISE_WARN      0.032f    // 訊噪比 30 dB
#define DUR_TOO_SHORT   0.35f     // 短於這個，包絡量不出衰減走勢
#define DUR_SHORT       0.90f     // config.h 的量測：1.0 秒開始，包絡相關性掉到 0.891

static void put(char *dst, size_t cap, const char *fmt, ...) {
  if (!dst || !cap) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dst, cap, fmt, ap);
  va_end(ap);
}

bool recCheckSnrKnown(float noiseFloor) { return noiseFloor > 1e-6f; }

float recCheckSnrDb(float noiseFloor) {
  if (!recCheckSnrKnown(noiseFloor)) return RECCHK_SNR_UNKNOWN;
  return -20.0f * log10f(noiseFloor);
}

const char *recVerdictTitle(RecVerdict v) {
  switch (v) {
    case REC_OK:   return "REC OK";
    case REC_WARN: return "REC - CHECK THIS";
    default:       return "REC FAILED";
  }
}

RecVerdict recCheckEval(const RecCheck &in,
                        char *reason, size_t reasonCap,
                        char *fix,    size_t fixCap) {
  if (reason && reasonCap) reason[0] = 0;
  if (fix    && fixCap)    fix[0]    = 0;

  // ---- 不能用 -------------------------------------------------------------
  //
  // 順序是按「使用者能多快改掉」排的，不是按嚴重程度。同時中兩條的時候，
  // 先講最好改的那一條 —— 一次只給一件事做，比列出三個問題有用。
  if (!in.analysisOk) {
    put(reason, reasonCap, "analysis failed");
    put(fix,    fixCap,    "play louder / longer");
    return REC_BAD;
  }

  if (in.onsets >= 2) {
    put(reason, reasonCap, "%d notes in one take", in.onsets);
    put(fix,    fixCap,    "play ONCE and hold");
    return REC_BAD;
  }

  if (in.clipRatio > CLIP_BAD) {
    put(reason, reasonCap, "clipped %.1f%%", in.clipRatio * 100.0f);
    put(fix,    fixCap,    "lower mic gain (g)");
    return REC_BAD;
  }

  // 「峰值低」本身不是問題，「峰值低而且埋在噪音裡」才是。
  //
  // 這一條原本只看峰值，結果把 Iowa MIS 的鋼琴原始素材判成不能用 ——
  // 那批檔案峰值只有 0.033，但訊噪比 40 dB，是拿來當基準的乾淨素材。
  // 錄音室母帶的絕對電平本來就可以很低，那是後製的事，不是品質問題。
  // 真正會毀掉分析的是量化雜訊爬進諧波裡，而那件事訊噪比看得到、峰值看不到。
  if (in.peak < PEAK_TOO_LOW &&
      recCheckSnrKnown(in.noiseFloor) && recCheckSnrDb(in.noiseFloor) < 26.0f) {
    put(reason, reasonCap, "too quiet: peak %.2f", in.peak);
    put(fix,    fixCap,    "raise gain or move in");
    return REC_BAD;
  }

  if (recCheckSnrKnown(in.noiseFloor) && in.noiseFloor > NOISE_BAD) {
    put(reason, reasonCap, "noisy: SNR %.0f dB", recCheckSnrDb(in.noiseFloor));
    put(fix,    fixCap,    "quieter room");
    return REC_BAD;
  }

  if (in.noteDur < DUR_TOO_SHORT) {
    put(reason, reasonCap, "note too short: %.2fs", in.noteDur);
    put(fix,    fixCap,    "hold the note 1-2 s");
    return REC_BAD;
  }

  // ---- 能用，但可以更好 ---------------------------------------------------
  if (in.peak > PEAK_HOT) {
    put(reason, reasonCap, "hot: peak %.2f", in.peak);
    put(fix,    fixCap,    "lower gain a bit (g)");
    return REC_WARN;
  }

  if (in.peak < PEAK_LOW) {
    put(reason, reasonCap, "quiet: peak %.2f", in.peak);
    put(fix,    fixCap,    "target peak 0.3 - 0.8");
    return REC_WARN;
  }

  if (recCheckSnrKnown(in.noiseFloor) && in.noiseFloor > NOISE_WARN) {
    put(reason, reasonCap, "SNR %.0f dB", recCheckSnrDb(in.noiseFloor));
    put(fix,    fixCap,    "noise floor audible");
    return REC_WARN;
  }

  if (in.noteDur < DUR_SHORT) {
    put(reason, reasonCap, "short: %.2fs", in.noteDur);
    put(fix,    fixCap,    "envelope may be cut");
    return REC_WARN;
  }

  // 衰減 >= 0.97 表示「這個音幾乎不衰減」。真的有這種樂器（管風琴、長音弦樂），
  // 所以不是錯誤 —— 但撥弦樂器量到這個值，八成是尾巴被切掉或錄到持續的噪音。
  // 只提醒，不判定，因為程式並不知道使用者手上拿的是什麼。
  if (in.decayPerSec >= 0.97f) {
    put(reason, reasonCap, "no decay measured");
    put(fix,    fixCap,    "ok for organ or bowed");
    return REC_WARN;
  }

  put(reason, reasonCap, "levels look good");
  put(fix,    fixCap,    "added to bank");
  return REC_OK;
}

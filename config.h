#pragma once

#include <Arduino.h>

#define TC_SAMPLE_RATE      44100.0f
#define TC_BLOCK            AUDIO_BLOCK_SAMPLES
#define TC_BLOCK_SEC        (TC_BLOCK / TC_SAMPLE_RATE)

#define TC_FFT_SIZE         2048
#define TC_HOP              512

#define TC_N_HARM           32
#define TC_N_PARTIAL        64

#define TC_MAX_FRAMES       512
#define TC_N_KEYFRAME       32

#define TC_ATK_WINDOW_SEC   0.30f
#define TC_ATK_HOP          128
#define TC_ATK_LP_HZ        120.0f
#define TC_SPECENV_PTS      64
#define TC_SPECENV_FMIN     50.0f
#define TC_SPECENV_FMAX     16000.0f

#define TC_F0_MIN           65.0f
#define TC_F0_MAX           1500.0f
#define TC_YIN_THRESH       0.15f

#define TC_N_VOICES         8

#ifndef TC_USE_USB_KBD
  #if defined(__IMXRT1062__)
    #define TC_USE_USB_KBD 1
  #else
    #define TC_USE_USB_KBD 0
  #endif
#endif

#define TC_SHIMMER_HZ_MIN   2.5f
#define TC_SHIMMER_HZ_MAX   7.5f

#ifndef TC_JITTER_CAL
#define TC_JITTER_CAL       1.9f

#endif

#define TC_TRANSPOSE_RESAMPLE   1

#define TC_TRANSPOSE_RESAMPLE_W 1.0f

#define TC_TRANSPOSE_RESAMPLE_LO 1.0f
#define TC_TRANSPOSE_RESAMPLE_HI 4.0f

#define TC_PARTIAL_BUDGET   320
#define TC_SINE_TBL_BITS    10
#define TC_SINE_TBL_SIZE    (1 << TC_SINE_TBL_BITS)
#define TC_NYQUIST_GUARD    0.45f

#ifndef TC_PEAK_WIDE
#define TC_PEAK_WIDE        1
#endif

#ifndef TC_SHIM_START_SEC
#define TC_SHIM_START_SEC   0.15f
#endif

#ifndef TC_SHIM_MAX
#define TC_SHIM_MAX         0.12f
#endif

#ifndef TC_JIT_SIGMA_MAX
#define TC_JIT_SIGMA_MAX    0.5774f
#endif

#ifndef TC_NOISE_LP_STAGES
#define TC_NOISE_LP_STAGES  3
#endif

#ifndef TC_NOISE_ROLL_DB
#define TC_NOISE_ROLL_DB    35.0f
#endif

#ifndef TC_NOISE_BB_GAIN
#define TC_NOISE_BB_GAIN    1.0f
#endif

#define TC_MLP_IN           4
#define TC_MLP_H1           32
#define TC_MLP_H2           32
#define TC_MLP_OUT          (TC_N_HARM + 1)

#define TC_MLP_PITCH_SCALE  1.0f

#ifndef TC_MLP_BLEND
#define TC_MLP_BLEND        0.0f
#endif

#ifndef TC_MLP_BLEND_AB
#define TC_MLP_BLEND_AB     0.35f
#endif

#define TC_MLP_MAGIC        0x324D4C50u
#define TC_PROFILE_MAGIC     0x31504355u

#define TC_MLP_NPARAM  (TC_MLP_H1 * TC_MLP_IN + TC_MLP_H1 +                  \
                        TC_MLP_H2 * TC_MLP_H1 + TC_MLP_H2 +                  \
                        TC_MLP_OUT * TC_MLP_H2 + TC_MLP_OUT)

#define TC_TRAIN_MAX        2560
#define TC_TRAIN_BATCH      128
#define TC_TRAIN_EPOCHS     6000
#define TC_TRAIN_LR         0.003f
#define TC_TRAIN_NOISE_W    0.3f

#define TC_REC_SECONDS      2
#define TC_REC_PATH         "REC.WAV"

#define TC_TRIG_LEVEL       0.035f

#define TC_TIMBRE_WARN_DIST     2.0f
#define TC_TIMBRE_WARN_MIN_REFS 3

#define TC_TRIG_BLOCKS      4

#define TC_TRIG_MARGIN      1.5f
#define TC_TRIG_CAL_MS      600

#define TC_TRIG_QUIET_MARGIN 1.20f
#define TC_TRIG_QUIET_FRAC   0.60f

#define TC_TRIG_MIN_HEADROOM_DB  12.0f
#define TC_PREROLL_BLOCKS   32

#define TC_REARM_SILENT_MS  400
#define TC_PROFILE_PATH     "PROFILE.BIN"
#define TC_MODEL_PATH       "MODEL.BIN"
#define TC_PLAY_PATH        "PLAY.WAV"
#define TC_CANON_PATH       "CANON.WAV"

#define TC_BANK_PATH        "BANK.BIN"

#define TC_MAX_SCAN_FILES   32
#define TC_MAX_NAME_LEN     32

#if defined(USB_MTPDISK) || defined(USB_MTPDISK_SERIAL)
#define TC_HAS_MTP          1
#else
#define TC_HAS_MTP          0
#endif

#ifndef TC_MTP_DEFAULT_ON
#define TC_MTP_DEFAULT_ON   1
#endif

#ifndef TC_USE_OLED
#define TC_USE_OLED         1
#endif

#define TC_OLED_SSD1306     1
#define TC_OLED_SH1106      0
#define TC_OLED_SSD1309     0

#define TC_OLED_I2C_HZ      400000
#define TC_OLED_REFRESH_MS  200

#define TC_SDCARD_CS_PIN    10
#define TC_SDCARD_MOSI_PIN  11
#define TC_SDCARD_SCK_PIN   13

#define TC_BTN_UP           2
#define TC_BTN_DOWN         3
#define TC_BTN_OK           4
#define TC_BTN_BACK         5

#define TC_BTN_DEBOUNCE_MS      20

#define TC_BTN_REPEAT_DELAY_MS  400
#define TC_BTN_REPEAT_MS        120

#define TC_KEY_PINS { 24, 34, 25, 35, 26, 27, 36, 28, 37, 29, 38, 30 }

#define TC_BPM              66.0f
#define TC_TICKS_PER_BEAT   4
#define TC_MAX_NOTES        224

static inline float tc_midiToHz(float midi) {
  return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
}
static inline int tc_clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline float tc_clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline float tc_tanh(float x) {
  if (x < -3.0f) return -1.0f;
  if (x >  3.0f) return  1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float tc_sigmoid(float x) {
  return 0.5f * (1.0f + tc_tanh(0.5f * x));
}

#define TC_TIME_WARP_TAU    0.06f
static inline float tc_timeWarp(float tSec, float noteDur) {
  if (noteDur < 1e-3f) noteDur = 1e-3f;
  if (tSec < 0.0f) tSec = 0.0f;
  const float k = 1.0f / TC_TIME_WARP_TAU;
  return logf(1.0f + k * tSec) / logf(1.0f + k * noteDur);
}

static inline int tc_partialCount(float f0) {
  if (f0 <= 1.0f) return 1;
  int n = (int)(TC_SAMPLE_RATE * TC_NYQUIST_GUARD / f0);
  if (n < 1) n = 1;
  if (n > TC_N_PARTIAL) n = TC_N_PARTIAL;
  return n;
}

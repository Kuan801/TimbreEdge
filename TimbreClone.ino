// ============================================================================
//  TimbreClone  -  Teensy 4.1 + Audio Shield (SGTL5000) + 128x64 OLED
//
//  Listen to a single note (a WAV on the SD card, or 5 s from the mic) ->
//  extract a timbre fingerprint -> rebuild that instrument with additive
//  synthesis + a small MLP -> play the four voices of the Canon refrain.
//  The MLP can be trained on a PC, or directly on the Teensy.
//
//  ── Serial commands (115200, line ending must be New Line) ────────────
//    r            record 5 s from the mic -> REC.WAV, analyze automatically
//    a [file]     analyze a WAV (no name = REC.WAV)
//    d            list the SD root directory
//
//    n [target]   analyze and add to the training set. Target can be:
//                   (blank)         REC.WAV
//                   A.WAV           a single file
//                   A.WAV B.WAV ..  several files
//                   *               every WAV in the root
//                   SAMPLES/        every WAV in that folder
//    t [epochs]   train the MLP on the Teensy, then save MODEL.BIN and apply it
//    z            clear the training set
//
//    p            play the Canon refrain (all of it from additive synthesis)
//    w            play and record to PLAY.WAV at the same time
//    x            stop
//
//    s            continuous sampling mode (recording triggers itself)
//    l            reload PROFILE.BIN
//    m            reload MODEL.BIN
//    k            A/B: toggle the MLP blend weight between 0 and TC_MLP_BLEND_AB
//    i            switch input source (mic / line in)
//    u            USB MTP on/off (mount the SD as a PC drive; needs the matching USB Type)
//    c            toggle FRAMES.CSV output
//    +  -         master volume
//    ?            help
//    PC keyboard  plug into the USB Host port. ZSXDCVGBHNJM = C4~B4,
//                  Q2W3ER5T6Y7U = C5~B5, arrow keys change octave / drive the menu (see kbd_in.h)
//
//  ── Wiring ─────────────────────────────────────────────────────────────
//    Audio Shield  stacked on the Teensy 4.1
//    OLED (I2C)    SDA = 18,  SCL = 19,  VCC = 3.3V,  GND = GND
//                  (shares I2C with the SGTL5000; the addresses do not clash: OLED 0x3C / codec 0x0A)
//    Buttons (opt) D2 = record and analyze   D3 = play / stop
// ============================================================================

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include "config.h"
#include "wav_io.h"
#include "profile.h"
#include "recorder.h"
#include "analyzer.h"
#include "timbre_model.h"
#include "additive_synth.h"
#include "player.h"
#include "trainer.h"
#include "display.h"
#include "rec_check.h"
#include "ui.h"
#include "buttons.h"
#include "keys.h"
#include "kbd_in.h"

// USB MTP: mount the SD as a drive on the PC. See TC_HAS_MTP in config.h for why it is conditional.
#if TC_HAS_MTP
#include <MTP_Teensy.h>
static bool gMtpOn = TC_MTP_DEFAULT_ON;
#endif

// ----------------------------------------------------------- Audio graph --
//   synth -> reverb -> mixer -> headphones
//                            \-> capture queue (recorded to PLAY.WAV)
//   What gets recorded is exactly what you hear, all of it additive synthesis,
//   with no sample playback anywhere.
AudioInputI2S            i2sIn;
AudioRecordQueue         recQueue;
AudioSynthAdditive       synth;
AudioEffectFreeverbStereo verb;
AudioMixer4              mixL;
AudioMixer4              mixR;
AudioRecordQueue         capQL;
AudioRecordQueue         capQR;
AudioOutputI2S           i2sOut;
AudioControlSGTL5000     sgtl;

AudioConnection pc0(i2sIn, 0, recQueue, 0);
AudioConnection pc1(synth, 0, verb, 0);
AudioConnection pc2(synth, 1, verb, 1);
AudioConnection pc3(synth, 0, mixL, 0);
AudioConnection pc4(verb,  0, mixL, 1);
AudioConnection pc5(synth, 1, mixR, 0);
AudioConnection pc6(verb,  1, mixR, 1);
AudioConnection pc7(mixL, 0, i2sOut, 0);
AudioConnection pc8(mixR, 0, i2sOut, 1);
AudioConnection pc9(mixL, 0, capQL, 0);
AudioConnection pcA(mixR, 0, capQR, 0);

// --------------------------------------------------------------- Modules --
InstrumentProfile gProfile;
DMAMEM ProfileBank gBank;      // Multi-pitch timbre bank (16 sets, roughly 75 KB)
TimbreModel       gModel;
Recorder          gRec;
StereoCapture     gCap;
Player            gPlayer;
TrainSet          gTrainSet;

static void uiRenderFwd();          // Defined down in the menu-tree section (so showIdle() can already use it)
static void uiHandleKey(UiKey k);   // Cancel on a confirmation page has to be able to send a Back key itself
// doAnalyze() needs this for the auto-play after recording, but it is defined further down.
// Arduino inserts function prototypes for you, g++ (tools/sim's inocheck) does not -- so
// hand it one line here.
// The default argument may only appear on this declaration (C++ forbids giving a default
// twice in the same scope), so the definition below no longer writes = TC_SCORE_SCALE.
static void startPlay(bool capture, ScoreMode mode = TC_SCORE_SCALE);

// Key-mode transpose, in octaves. The menu's "Key octave" item edits it directly.
//
// It sits here rather than with the other gUi* in the menu-tree section because showIdle()
// uses it to show the current octave, and showIdle() is in the upper half of the file.
// Arduino's preprocessing only auto-inserts "function prototypes", never variable
// declarations -- a variable placed after its use is simply 'not declared in this scope'.
static int16_t gUiOctave = 0;

DMAMEM MlpWeights gTrainedW;                                   // Training output
DMAMEM char       gScanNames[TC_MAX_SCAN_FILES][TC_MAX_NAME_LEN];  // Batch scan

bool  gDumpCsv  = false;
bool  gAutoPlay = false;      // Record -> analyze -> auto-play
bool  gCapture  = false;      // Whether this playback gets recorded to PLAY.WAV
float gVolume   = 0.55f;
bool  gMicInput = true;
bool  gSdOk     = false;
bool  gSampling = false;      // Continuous sampling mode
int   gSampCount = 0;         // How many notes this session has collected
// The folder for this round of sampling, e.g. "SET03". Empty means it could not be created, so fall back to the root.
char  gSampDir[12] = "";
// The most recent sampling folder that actually caught something. gSampDir is cleared when
// the session ends, but "load the set I just recorded" has to remember it afterwards, so
// keep a second copy.
char  gLastSet[12] = "";
int   gMicGain  = 36;         // MIC-mode preamp gain, 0~63 dB
// LINE IN input sensitivity, 0~15 (SGTL5000's convention: bigger number = smaller full-scale
// voltage = more sensitive). This used to be hard-coded to 5 (about 1.33 Vp-p), which suits
// line-level source gear; a phone or laptop headphone jack puts out far less, so a fixed
// value meant recording at a very low level.
// Now it is adjustable just like micGain -- LINE IN is the cleanest way to bypass the speaker
// and the room, and for that path to be usable the gain has to be movable.
int   gLineLevel = 5;
bool  gMonitor  = false;      // Input level monitor
float gSampThresh = TC_TRIG_LEVEL;

char    gLine[96];
uint8_t gLineLen = 0;

// ============================================ OLED progress callbacks =====
static void onAnalyzeProgress(float frac) {
  displaySetProgress(frac);
  displayForce();
}
static void onTrainProgress(int ep, int total, float ce, float mae) {
  char b[26];
  displaySetProgress(total ? (float)ep / total : 0.0f);
  snprintf(b, sizeof(b), "epoch %d/%d", ep, total);   displaySetLine(0, b);
  snprintf(b, sizeof(b), "mae %.5f", mae);            displaySetLine(1, b);
  snprintf(b, sizeof(b), "CE  %.4f", ce);             displaySetLine(2, b);
  displayForce();
}

static void refreshDisplayStatus() {
  displaySetSystem(gSdOk, gModel.mlpActive(), gProfile.valid);
  displaySetTrainInfo(gTrainSet.size(), gTrainSet.pitchCount());
  if (gProfile.valid) {
    // f0 -> note name, easier to read on the panel than Hz
    static const char *kNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    int midi = (int)lroundf(69.0f + 12.0f * log2f(gProfile.f0 / 440.0f));
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    char nm[8];
    snprintf(nm, sizeof(nm), "%s%d", kNames[((midi % 12) + 12) % 12], midi / 12 - 1);
    displaySetProfileInfo(gProfile.f0, nm);
  } else {
    displaySetProfileInfo(0.0f, "");
  }
}

static void showIdle() {
  refreshDisplayStatus();
  displaySetState(TC_ST_IDLE);
  char b[26];
  if (gProfile.valid) {
    snprintf(b, sizeof(b), "A%.2f D%.2f S%.2f", gProfile.attack, gProfile.decay,
             gProfile.sustain);
    displaySetLine(0, b);
    snprintf(b, sizeof(b), "harm %d  noise %.2f", TC_N_HARM, gProfile.noiseGain);
    displaySetLine(1, b);
  } else {
    displaySetLine(0, "no timbre loaded");
    displaySetLine(1, "press r  or  a FILE");
  }
  snprintf(b, sizeof(b), "train %d smp / %d pit", gTrainSet.size(), gTrainSet.pitchCount());
  displaySetLine(2, b);
  // Line 4 shows whether the PC keyboard on the USB Host port is online.
  if (gKbd.connected()) snprintf(b, sizeof(b), "PC kbd ready  oct%+d", (int)gUiOctave);
  else                  snprintf(b, sizeof(b), "USB: no keyboard");
  displaySetLine(3, b);
  displayForce();
  uiRenderFwd();     // Content is ready; it decides whether the screen goes to the menu or the status panel
}

// ---------------------------------------------------------------------------
//  Short-lived result notices
//
//  If "did that do anything" is only printed to the serial port, the person standing in
//  front of the device sees nothing at all. Irreversible operations -- clearing the timbre
//  bank, deleting files -- need feedback most of all: without it the user is not sure it
//  took effect, and presses again.
//
//  Designed to "show for a few seconds, then fall back to idle" rather than "stay until a
//  key is pressed": a success message should not demand another action from the user.
//  Any key still dismisses it early.
//
//  Error screens (multiple attacks, for instance) deliberately do not use this -- those
//  have to stay up until somebody has seen them.
// ---------------------------------------------------------------------------
static uint32_t gNoticeUntil = 0;

// What happens after a notice goes away (on its own, or because it was dismissed).
//
// The recording-verdict panel is the only one with a follow-up: if the take passes, play
// once automatically so the synthesized result can be heard immediately. It used to play
// the moment analysis finished, which left no time to see the verdict panel at all -- so
// the fix was "let it be read first, then play", not dropping the playback.
static void noticeDismissed() {
  if (gAutoPlay) { gAutoPlay = false; startPlay(false); return; }
  showIdle();
}

static void showNotice(const char *title, const char *l0, const char *l1,
                       const char *l2, uint32_t ms = 2500) {
  displaySetState(TC_ST_IDLE, title);      // Use IDLE so the menu is not blocked from coming back
  displaySetProgress(-1.0f);
  displaySetLine(0, l0 ? l0 : "");
  displaySetLine(1, l1 ? l1 : "");
  displaySetLine(2, l2 ? l2 : "");
  displaySetLine(3, "");
  displaySetMenu(nullptr, nullptr, 0, 0, 0, 0, false);   // Push the menu aside for now
  displayForce();
  gNoticeUntil = millis() + ms;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Codec self-check: read the SGTL5000 registers back
//
//  Why this is needed: inputSelect() / micGain() both return bool, but that only proves
//  "the I2C write went out". When the mic gives no signal at all, what you actually want
//  to know is "what state is the chip in right now" -- is the input MIC or LINE IN, what
//  is the gain really set to, is the ADC muted, is the bias supplied. These few registers
//  split the problem in two in one go:
//
//      cannot read / wrong values  -> the software setting never took, or I2C is simply
//                                     dead (check SDA18/SCL19)
//      values all correct          -> the settings are fine, the problem is on the analog
//                                     side (mic, solder joints, wiring)
//
//  Without this there is nothing to do but guess, and the two cases are handled in
//  completely different ways.
//
//  On the Audio Shield the SGTL5000 lives at I2C address 0x0A; register addresses are
//  16-bit big-endian, and so is the data. Bit definitions taken from PJRC's
//  control_sgtl5000.cpp.
// ---------------------------------------------------------------------------
#define SGTL_ADDR           0x0A
#define SGTL_CHIP_ID        0x0000
#define SGTL_ANA_ADC_CTRL   0x0020
#define SGTL_ANA_CTRL       0x0024
#define SGTL_MIC_CTRL       0x002A

static bool sgtlRead(uint16_t reg, uint16_t &val) {
  Wire.beginTransmission((uint8_t)SGTL_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;      // Repeated start, do not release the bus
  if (Wire.requestFrom((uint8_t)SGTL_ADDR, (uint8_t)2) < 2) return false;
  const uint8_t hi = (uint8_t)Wire.read();
  const uint8_t lo = (uint8_t)Wire.read();
  val = (uint16_t)((hi << 8) | lo);
  return true;
}

static void sgtlDump() {
  uint16_t id = 0;
  if (!sgtlRead(SGTL_CHIP_ID, id)) {
    Serial.println(F("[CODEC] I2C 0x0A 沒有回應 —— 編解碼器根本沒被設定到。"));
    Serial.println(F("        這種狀況輸出也會是無聲。先查 Audio Shield 的"));
    Serial.println(F("        SDA(18)/SCL(19)/3.3V/GND 有沒有接好、有沒有虛焊。"));
    return;
  }
  const uint8_t part = (uint8_t)(id >> 8);
  Serial.printf("[CODEC] CHIP_ID 0x%04X（PARTID 0x%02X %s，REV 0x%02X）\n",
                id, part, (part == 0xA0) ? "= SGTL5000，正常" : "不是 SGTL5000？",
                (uint8_t)(id & 0xFF));

  uint16_t ana = 0, mic = 0, adc = 0;
  if (sgtlRead(SGTL_ANA_CTRL, ana)) {
    Serial.printf("[CODEC] ANA_CTRL 0x%04X -> ADC 輸入 %s，ADC %s\n", ana,
                  (ana & 0x0004) ? "LINE IN" : "MIC",
                  (ana & 0x0001) ? "**靜音中**" : "未靜音");
  }
  if (sgtlRead(SGTL_MIC_CTRL, mic)) {
    static const uint8_t kPre[4] = { 0, 20, 30, 40 };
    static const char   *kRes[4] = { "**關閉（沒有偏壓）**", "2.0k", "4.0k", "8.0k" };
    Serial.printf("[CODEC] MIC_CTRL 0x%04X -> 前級 %d dB，偏壓 %.2f V / %s\n", mic,
                  kPre[mic & 0x3],
                  1.25f + 0.25f * ((mic >> 4) & 0x7),
                  kRes[(mic >> 8) & 0x3]);
  }
  if (sgtlRead(SGTL_ANA_ADC_CTRL, adc)) {
    const float step = (adc & 0x0100) ? -6.0f : 0.0f;
    Serial.printf("[CODEC] ADC_CTRL 0x%04X -> ADC 增益 左 %.1f dB / 右 %.1f dB\n", adc,
                  step + 1.5f * (adc & 0xF), step + 1.5f * ((adc >> 4) & 0xF));
  }
  if (sgtlRead(SGTL_MIC_CTRL, mic) && sgtlRead(SGTL_ANA_ADC_CTRL, adc)) {
    static const uint8_t kPre[4] = { 0, 20, 30, 40 };
    const float total = kPre[mic & 0x3] + 1.5f * (adc & 0xF)
                      + ((adc & 0x0100) ? -6.0f : 0.0f);
    Serial.printf("[CODEC] 麥克風總增益 %.1f dB（上限 62.5 dB，打「g 63」拉到底）\n", total);
  }
}

static void applyInput() {
  // These all return "did the I2C write succeed". The return value used to be dropped on
  // the floor -- so a dead bus gave no sign at all, and only showed up as "the mic picks
  // up nothing".
  const bool okSel  = sgtl.inputSelect(gMicInput ? AUDIO_INPUT_MIC : AUDIO_INPUT_LINEIN);
  const bool okGain = gMicInput ? sgtl.micGain(gMicGain) : sgtl.lineInLevel(gLineLevel);
  if (!okSel || !okGain)
    Serial.println(F("[CODEC] ! 設定寫不進 SGTL5000（I2C 失敗）。按 o 看完整自我檢查。"));

  // ---- ADC high-pass filter: measure the DC offset, then freeze -----------
  //
  // This was once adcHighPassFilterDisable(), on the reasoning that "the filter reaches
  // into the very bottom of the band, and the whole timbre analysis rests on the relative
  // amplitudes of the harmonics". The motivation was right, the function was the wrong one:
  // Disable maps to ADC_HPF_BYPASS, and PJRC's comment says so plainly --
  // "Bypassed and offset not updated", i.e. **no DC is subtracted at all**.
  //
  // What that did in practice: the DC offset of the MIC path turned into a fixed pedestal.
  // The monitor showed RMS 0.026, 96~97% of the energy below 300 Hz, and it did not budge
  // whether or not anything was played; the trigger threshold is computed as "ambient noise
  // x margin", so that pedestal lifted it, and the symptom read as "the mic is very
  // insensitive / sampling never triggers". The analysis side was hit just as hard:
  // analyzer does no mean removal, so DC contaminates the envelope and the lowest few FFT
  // bins directly.
  //
  // The right thing is Freeze (ADC_HPF_FREEZE):
  //   "Freeze the ADC high-pass filter offset register.
  //    The offset continues to be subtracted from the ADC data stream."
  // DC is still subtracted, but the filter stops adjusting -- so the thing we were worried
  // about, "the filter moving the signal", never happens.
  //
  // Order matters: the offset depends on the gain, so it has to be re-measured every time
  // the input or the gain changes. Let it run until the offset converges, then freeze.
  sgtl.adcHighPassFilterEnable();
  delay(400);                       // Let the offset converge (only happens when a setting changes)
  sgtl.adcHighPassFilterFreeze();
  if (gMicInput) Serial.printf("[IN] 輸入來源：麥克風   micGain %d dB\n", gMicGain);
  else           Serial.printf("[IN] 輸入來源：線性輸入 lineInLevel %d\n", gLineLevel);
}

static void printHelp() {
  Serial.println();
  Serial.println(F("========== TimbreClone 指令 =========="));
  Serial.println(F("  r           麥克風錄一段並自動分析"));
  Serial.println(F("  s [門檻]    連續採樣模式（可指定觸發門檻，例如 s 0.01）"));
  Serial.println(F("  g [0-63]    設定麥克風增益（不給數字就只顯示目前值）"));
  Serial.println(F("  o           輸入電平監看（麥克風沒反應時先用這個）"));
  Serial.println(F("  v           顯示音色庫的音域覆蓋"));
  Serial.println(F("  y y         刪掉根目錄程式產生的音檔（REC/PLAY/音名檔），素材不動"));
  Serial.println(F("  y s [SETnn] 刪掉採樣資料夾。不給名字 = 全部刪"));
  Serial.println(F("  a [檔名]    分析 WAV（預設 REC.WAV）"));
  Serial.println(F("  d           列出 SD 根目錄"));
  Serial.println(F("  --- 機上訓練 ---"));
  Serial.println(F("  n [目標]    分析並加入訓練集"));
  Serial.println(F("              n            -> REC.WAV"));
  Serial.println(F("              n A.WAV B.WAV-> 多個檔案"));
  Serial.println(F("              n *          -> 根目錄所有 WAV"));
  Serial.println(F("              n SAMPLES/   -> 該資料夾所有 WAV"));
  Serial.println(F("              n SET        -> 最近一輪連續採樣的 SETnn 資料夾"));
  Serial.println(F("  t [epochs]  在 Teensy 上訓練，完成後自動存 MODEL.BIN"));
  Serial.println(F("  z           清空訓練集"));
  Serial.println(F("  --- 演奏 ---"));
  Serial.println(F("  p           演奏半音階（音域跟著音色庫走，逐音可比對）"));
  Serial.println(F("  w           演奏並同步錄成 PLAY.WAV"));
  Serial.println(F("  j [n]       演奏卡農（三聲部，拿來聽的）並錄成 CANON.WAV"));
  Serial.println(F("              j n -> 只演奏不錄檔"));
  Serial.println(F("  x           停止"));
  Serial.println(F("  --- 其他 ---"));
  Serial.println(F("  l           重新載入 PROFILE.BIN（分析完會自動存，不用手動存）"));
  Serial.println(F("  m           重新載入 MODEL.BIN"));
  Serial.printf ("  k           A/B：MLP 權重 0 <-> %.2f（目前 %.2f）\n",
                 TC_MLP_BLEND_AB, gModel.blend());
  Serial.println(F("  i           切換麥克風 / 線性輸入"));
#if TC_HAS_MTP
  Serial.printf ("  u           USB MTP 磁碟機開關（目前 %s）\n", gMtpOn ? "開" : "關");
#endif
  Serial.println(F("  c           切換是否輸出 FRAMES.CSV"));
  Serial.println(F("  + -         主音量"));
  Serial.println(F("  ?           這份說明"));
  Serial.println(F("---------- USB 電腦鍵盤（插上 USB Host 埠即可，不用下指令）----------"));
  Serial.println(F("  Z S X D C V G B H N J M   C4 ~ B4"));
  Serial.println(F("  Q 2 W 3 E R 5 T 6 Y 7 U   C5 ~ B5"));
  Serial.println(F("  左右方向鍵  換八度        空白鍵  全部停音"));
  Serial.println(F("  上下 / Enter / Esc        操作 OLED 選單"));
  Serial.println(F("======================================"));
}

// ---------------------------------------------------------------------------
static const char *kNoteNames[12] = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};

static void midiToName(int m, char *out, size_t cap) {
  if (m < 0) m = 0;
  if (m > 127) m = 127;
  snprintf(out, cap, "%s%d", kNoteNames[((m % 12) + 12) % 12], m / 12 - 1);
}

// Delete the audio files the program generated itself. Timbre data (BANK/PROFILE/MODEL) is
// left alone -- that is a different matter; to clear timbres, use Clear trainset.
static void purgeGeneratedWavs() {
  if (!gSdOk) {
    Serial.println(F("[SD] 卡片沒掛起來，沒東西可刪。按 d 重試掛載。"));
    showNotice("NO SD CARD", "cannot delete", "press d to remount", "");
    return;
  }

  // Sweep once to collect the filenames, then delete. Deleting directory entries while
  // iterating over them is undefined on FAT, and in practice misses half the files.
  static char victims[TC_MAX_SCAN_FILES][TC_MAX_NAME_LEN];
  int nv = 0;
  uint32_t freed = 0;

  File dir = SD.open("/");
  if (!dir) { Serial.println(F("[SD] 打不開根目錄")); return; }
  while (nv < TC_MAX_SCAN_FILES) {
    File e = dir.openNextFile();
    if (!e) break;
    const char *nm = e.name();
    if (nm && nm[0] != '.' && !e.isDirectory() && tcIsGeneratedWav(nm)) {
      snprintf(victims[nv], TC_MAX_NAME_LEN, "%s", nm);
      freed += e.size();
      nv++;
    }
    e.close();
  }
  dir.close();

  if (nv == 0) {
    Serial.println(F("[SD] 沒有找到程式產生的音檔，不用清。"));
    Serial.println(F("     （使用者自己放的素材有前綴，例如 Piano.mf.C4.wav，不在清除範圍內）"));
    showNotice("NOTHING TO DELETE", "no generated WAV", "your samples kept", "");
    return;
  }

  Serial.printf("[SD] 準備刪除 %d 個檔（共 %lu KB）：\n", nv, (unsigned long)(freed / 1024));
  int done = 0;
  for (int i = 0; i < nv; i++) {
    const bool ok = SD.remove(victims[i]);
    Serial.printf("  %-24s %s\n", victims[i], ok ? "已刪除" : "**刪除失敗**");
    if (ok) done++;
  }
  Serial.printf("[SD] 完成，刪掉 %d / %d 個，釋放約 %lu KB\n",
                done, nv, (unsigned long)(freed / 1024));
  Serial.println(F("     音色庫與訓練集沒有動 —— 那個要用 Clear trainset。"));

  char b0[26], b1[26], b2[26];
  snprintf(b0, sizeof(b0), "deleted %d / %d files", done, nv);
  snprintf(b1, sizeof(b1), "freed %lu KB", (unsigned long)(freed / 1024));
  snprintf(b2, sizeof(b2), "%s", (done == nv) ? "timbre bank kept"
                                              : "!! some failed");
  showNotice("WAV FILES DELETED", b0, b1, b2, 3000);
}

static void bankCoverage();   // Defined below; endSampSession() uses it first

// ---------------------------------------------------------------------------
//  Sampling teardown. Both the serial port and the menu end up here.
//
//  There used to be one copy on each side, differing only in the serial one also calling
//  gTrainSet.summary(). Two nearly identical teardowns like that drift apart sooner or
//  later -- and now that "copy BANK.BIN into the folder" has been added, there is even
//  less reason to keep two versions.
static void endSampSession() {
  gRec.endSession();
  gSampling = false;

  Serial.printf("[SAMP] 這次共採到 %d 個音\n", gSampCount);
  bankCoverage();
  gTrainSet.summary();

  if (gSdOk && gBank.n > 0) {
    bankSave(gBank, TC_BANK_PATH);
    // Keep a copy in the folder too, so one set of samples is self-contained: the audio
    // files plus the timbre bank computed from those files. To go back to this set later,
    // just copy it into the root; no need to run the analysis again.
    if (gSampDir[0] && gSampCount > 0) {
      char p[32];
      snprintf(p, sizeof(p), "%s/%s", gSampDir, TC_BANK_PATH);
      if (bankSave(gBank, p))
        Serial.printf("[SAMP] 音檔與音色庫都在 %s/，要重新載入就打「n %s/」"
                      "或選單的 Load newest SET\n", gSampDir, gSampDir);
      snprintf(gLastSet, sizeof(gLastSet), "%s", gSampDir);
    }
  }
  gSampDir[0] = 0;
}

// ---------------------------------------------------------------------------
//  Delete sampling folders
//
//  arg "*" deletes them all, "SET02" deletes only that one.
//  Timbre data (BANK/PROFILE/MODEL in the root) is left alone -- that is a different matter.
static void purgeSampleSets(const char *which) {
  if (!gSdOk) {
    Serial.println(F("[SD] 卡片沒掛起來。按 d 重試掛載。"));
    showNotice("NO SD CARD", "cannot delete", "press d to remount", "");
    return;
  }

  static char sets[TC_MAX_SCAN_FILES][TC_MAX_NAME_LEN];
  const int n = tcSdCollectSets(&sets[0][0], TC_MAX_SCAN_FILES);
  if (n == 0) {
    Serial.println(F("[SD] 卡上沒有採樣資料夾（SETnn）。"));
    showNotice("NO SAMPLE SETS", "nothing to delete", "", "");
    return;
  }

  int totalFiles = 0, totalSets = 0;
  uint32_t totalBytes = 0;
  const bool all = (which && which[0] == '*');

  for (int i = 0; i < n; i++) {
    if (!all && !(which && strcasecmp(sets[i], which) == 0)) continue;
    int nf = 0; uint32_t fb = 0;
    const bool ok = tcSdRemoveDir(sets[i], &nf, &fb);
    Serial.printf("  %-8s %2d 個檔  %4lu KB  %s\n",
                  sets[i], nf, (unsigned long)(fb / 1024), ok ? "已刪除" : "**部分失敗**");
    totalFiles += nf; totalBytes += fb; totalSets++;
  }

  if (totalSets == 0) {
    Serial.printf("[SD] 找不到 %s\n", which ? which : "(空)");
    showNotice("SET NOT FOUND", which ? which : "", "", "");
    return;
  }

  Serial.printf("[SD] 刪掉 %d 組、%d 個檔，釋放約 %lu KB\n",
                totalSets, totalFiles, (unsigned long)(totalBytes / 1024));
  Serial.println(F("     根目錄的音色庫與訓練集沒有動。"));

  char b0[26], b1[26];
  snprintf(b0, sizeof(b0), "deleted %d set(s)", totalSets);
  snprintf(b1, sizeof(b1), "%d files, %lu KB", totalFiles,
           (unsigned long)(totalBytes / 1024));
  showNotice("SAMPLE SETS DELETED", b0, b1, "timbre bank kept", 3000);
}

// Range coverage of the timbre bank: which semitones are there and which are still missing
static void bankCoverage() {
  if (gBank.n == 0) { Serial.println(F("[BANK] 還沒有任何音色")); return; }
  int lo = 127, hi = 0;
  int have[128] = {0};
  for (int i = 0; i < gBank.n; i++) {
    int m = (int)lroundf(69.0f + 12.0f * log2f(gBank.p[i].f0 / 440.0f));
    if (m < 0 || m > 127) continue;
    have[m] = 1;
    if (m < lo) lo = m;
    if (m > hi) hi = m;
  }
  char a[8], b[8];
  midiToName(lo, a, sizeof(a));
  midiToName(hi, b, sizeof(b));
  Serial.printf("[BANK] %d 組，音域 %s ~ %s：", gBank.n, a, b);
  for (int m = lo; m <= hi; m++) {
    char nm[8];
    midiToName(m, nm, sizeof(nm));
    Serial.printf("%s%s ", have[m] ? "" : "(", nm);
    if (!have[m]) Serial.print(F(")"));
  }
  Serial.println();
  int gaps = 0;
  for (int m = lo; m <= hi; m++) if (!have[m]) gaps++;
  if (gaps) Serial.printf("       括號內的 %d 個半音沒採到，會用鄰近的音色移調頂替\n", gaps);
}

// ============================================ Analysis / training set =====
static bool analyzeOne(const char *path, bool addToTrainSet) {
  if (!gSdOk) {
    Serial.println(F("[ANA] SD 卡沒掛起來，任何檔案都讀不到。按 d 重試掛載。"));
    return false;
  }
  if (!SD.exists(path)) {
    Serial.printf("[ANA] SD 上沒有 %s\n", path);
    if (strcmp(path, TC_REC_PATH) == 0)
      Serial.println(F("      REC.WAV 是按 r 錄音後才會產生的。"
                       "想用現成音檔請輸入「a 你的檔名.WAV」，或按 d 看卡上有什麼。"));
    else
      Serial.println(F("      按 d 列出根目錄，確認檔名拼寫與大小寫。"));
    return false;
  }

  // Analysis blocks, and nobody services the keyboard while it runs. Release the held notes
  // first, or a pile of stuck notes will still be sounding when it comes back.
  gKbd.allOff();
  displaySetState(TC_ST_ANALYZING, path);
  displaySetProgress(0.0f);
  displayForce();

  const char *csv = gDumpCsv ? "FRAMES.CSV" : nullptr;
  const int before = gTrainSet.size();

  bool ok = analyzeWavFile(path, gProfile, csv, addToTrainSet ? &gTrainSet : nullptr);
  if (!ok) { Serial.println(F("[ANA] 分析失敗")); return false; }

  gModel.setProfile(&gProfile);
  profileSave(gProfile, TC_PROFILE_PATH);
  // Every analyzed file goes straight into the timbre bank; each note then picks the set closest in pitch
  if (gBank.add(gProfile)) { gBank.summary(); if (gSdOk) bankSave(gBank, TC_BANK_PATH); }
  if (gDumpCsv) Serial.println(F("[ANA] 已輸出 FRAMES.CSV"));
  if (addToTrainSet) Serial.printf("[TRAIN] 加入 %d 筆樣本\n", gTrainSet.size() - before);
  return true;
}

// ---------------------------------------------------------------------------
//  Verdict panel for a single-note recording
//
//  Analysis used to stop on screen only for "multiple attacks"; everything else went
//  straight past. The result: a bad take (clipped, too quiet, triggered early by noise)
//  gave no sign at all, and you only noticed when the synthesis sounded wrong and went
//  back to guess file by file. That is exactly how those two REC.WAVs recorded off a phone
//  speaker were wasted.
//
//  Now it pauses whether it passed or failed, and puts up four numbers plus one line of
//  "what to change". The verdict logic is in rec_check.cpp (pure numbers, tested on the
//  desktop).
//
//    line 1  note name / f0 / duration
//    line 2  peak / SNR / decay
//    line 3  verdict
//    line 4  what to do next
// ---------------------------------------------------------------------------
static RecVerdict showRecResult(bool analysisOk) {
  RecCheck rc;
  rc.analysisOk  = analysisOk;
  rc.peak        = analyzerLastPeak();
  rc.clipRatio   = analyzerLastClipRatio();
  rc.noiseFloor  = analyzerLastNoiseFloor();
  rc.onsets      = analyzerLastOnsetCount();
  rc.noteDur     = analysisOk ? gProfile.noteDur : 0.0f;
  rc.f0          = analysisOk ? gProfile.f0      : 0.0f;
  rc.decayPerSec = analysisOk ? gProfile.sustainDecayPerSec : 0.0f;

  char reason[26], fix[26];
  const RecVerdict v = recCheckEval(rc, reason, sizeof(reason), fix, sizeof(fix));

  char b[26];
  // A failed verdict uses the ERROR state: it stays on screen waiting for a key press and
  // does not vanish by itself. A pass uses a notice -- a few seconds is enough, do not hold
  // up the next step.
  if (v == REC_BAD) displaySetState(TC_ST_ERROR, recVerdictTitle(v));
  else              displaySetState(TC_ST_IDLE,  recVerdictTitle(v));
  displaySetProgress(-1.0f);

  if (analysisOk) {
    const int midi = (int)lroundf(69.0f + 12.0f * log2f(gProfile.f0 / 440.0f));
    char nm[8];
    midiToName(midi, nm, sizeof(nm));
    snprintf(b, sizeof(b), "%s %.0fHz  %.2fs", nm, gProfile.f0, gProfile.noteDur);
  } else {
    snprintf(b, sizeof(b), "no pitch detected");
  }
  displaySetLine(0, b);

  // Show "--" when the SNR cannot be measured, rather than printing a nice-looking number --
  // that would make the recording look clean, when in fact there simply was no stretch up
  // front to use as a noise floor.
  if (recCheckSnrKnown(rc.noiseFloor))
    snprintf(b, sizeof(b), "pk%.2f snr%2.0f dec%.2f",
             rc.peak, recCheckSnrDb(rc.noiseFloor), rc.decayPerSec);
  else
    snprintf(b, sizeof(b), "pk%.2f snr-- dec%.2f", rc.peak, rc.decayPerSec);
  displaySetLine(1, b);
  displaySetLine(2, reason);
  displaySetLine(3, fix);

  // The serial port gets the full version; the panel, limited to 21 characters, gets only the verdict
  Serial.printf("[CHK] %s：%s / %s\n", recVerdictTitle(v), reason, fix);
  Serial.printf("      峰值 %.3f  削波 %.3f%%  訊噪比 ", rc.peak, rc.clipRatio * 100.0f);
  if (recCheckSnrKnown(rc.noiseFloor)) Serial.printf("%.0f dB", recCheckSnrDb(rc.noiseFloor));
  else                                 Serial.print(F("量不到（起音在錄音的最開頭）"));
  Serial.printf("  起音 %d 次  音長 %.2f 秒  衰減 %.2f/秒\n",
                rc.onsets, rc.noteDur, rc.decayPerSec);

  if (v == REC_BAD) {
    displaySetMenu(nullptr, nullptr, 0, 0, 0, 0, false);
    displayForce();
    gNoticeUntil = 0;              // Does not clear itself, waits for a key press
  } else {
    displaySetMenu(nullptr, nullptr, 0, 0, 0, 0, false);
    displayForce();
    gNoticeUntil = millis() + 4000;
  }
  return v;
}

static void doAnalyze(const char *path) {
  if (gPlayer.playing()) gPlayer.stop();
  synth.allNotesOff();
  const bool ok = analyzeOne(path, false);

  const RecVerdict v = showRecResult(ok);

  // Do not auto-play a failed take -- that would paint over the verdict just drawn, and the
  // verdict is the whole point.
  if (v == REC_BAD) { gAutoPlay = false; return; }

  // On a pass, auto-play only starts after the verdict panel has been up for 4 seconds (see
  // gNoticeUntil in loop()).
  //
  // Playback has to go through startPlay() rather than gPlayer.start() directly: the latter
  // skips three things -- switching the display to PLAYING, recomputing the scale range from
  // the timbre bank, and loading the score. Not switching the display is the worst of the
  // three: the state stays at ANALYZING, and uiRender() draws no menu when the state is not
  // IDLE, so the buttons clearly respond yet nothing appears -- which looks exactly like a
  // crash.
}

// The n command: single file, several files, wildcard, or a whole folder.
// "n SET" = load the most recent round of sampling. Deliberately only the bare "SET", with
// no digits and no slash: a real folder is always SETnn (tcSdMakeNextSet always pads to two
// digits), so it can never collide with a real file or folder name.
static bool isSetToken(const char *t) {
  return t && strcasecmp(t, TC_SET_PREFIX) == 0;
}

static void doAddToTrainSet(char *target) {
  if (gPlayer.playing()) gPlayer.stop();
  synth.allNotesOff();

  int okCount = 0, failCount = 0;

  auto addPath = [&](const char *p) {
    if (analyzeOne(p, true)) okCount++; else failCount++;
  };

  if (!*target) {
    addPath(TC_REC_PATH);

  } else if (target[0] == '*' || strchr(target, '*')) {
    // Scan the whole root
    int n = tcSdCollectWavs("/", &gScanNames[0][0], TC_MAX_SCAN_FILES, TC_PLAY_PATH);
    Serial.printf("[TRAIN] 根目錄找到 %d 個 WAV\n", n);
    for (int i = 0; i < n; i++) {
      Serial.printf("  (%d/%d) %s\n", i + 1, n, gScanNames[i]);
      addPath(gScanNames[i]);
    }
    // Continuous sampling writes into SETnn/, not the root. Someone who has just finished a
    // whole round but only sees REC.WAV will think it is broken -- so list the folders here
    // as well.
    {
      static char sets[TC_MAX_SCAN_FILES][TC_MAX_NAME_LEN];
      const int ns = tcSdCollectSets(&sets[0][0], TC_MAX_SCAN_FILES);
      if (ns > 0) {
        Serial.printf("[TRAIN] 另外有 %d 組採樣資料夾（n * 不會掃到它們）：", ns);
        for (int i = 0; i < ns; i++) Serial.printf(" %s", sets[i]);
        Serial.printf("\n        要載入請打「n %s/」，或用選單的 Load newest SET\n",
                      sets[ns - 1]);
      }
    }

  } else if (isSetToken(target)) {
    // "The most recent set". The menu cannot let anyone type, so this is its only route.
    //
    // What "most recent" means: the set recorded since this power-up if there is one
    // (gLastSet); otherwise fall back to the highest number. The number alone is not the
    // criterion, because tcSdMakeNextSet fills numbers the user deleted back in -- delete
    // SET02, sample another round, and the new set is called SET02, while the
    // highest-numbered SET03 is the older one.
    char pick[12] = "";
    const char *why = "";
    if (gLastSet[0]) {
      snprintf(pick, sizeof(pick), "%s", gLastSet);
      why = "這次開機採的那一組";
    } else {
      static char sets[TC_MAX_SCAN_FILES][TC_MAX_NAME_LEN];
      const int ns = tcSdCollectSets(&sets[0][0], TC_MAX_SCAN_FILES);
      if (ns > 0) {
        snprintf(pick, sizeof(pick), "%s", sets[ns - 1]);
        why = "編號最大的那一組（這次開機沒有採樣）";
      }
    }
    if (!pick[0]) {
      Serial.println(F("[TRAIN] SD 上沒有任何 SETnn 採樣資料夾。先按 s 採一輪。"));
    } else {
      int n = tcSdCollectWavs(pick, &gScanNames[0][0], TC_MAX_SCAN_FILES, TC_PLAY_PATH);
      Serial.printf("[TRAIN] 載入 %s/（%s）：%d 個 WAV\n", pick, why, n);
      for (int i = 0; i < n; i++) {
        char full[TC_MAX_NAME_LEN * 2 + 2];
        snprintf(full, sizeof(full), "%s/%s", pick, gScanNames[i]);
        Serial.printf("  (%d/%d) %s\n", i + 1, n, full);
        addPath(full);
      }
    }

  } else if (target[strlen(target) - 1] == '/') {
    // Scan a whole named folder
    char dir[sizeof(gLine)];
    snprintf(dir, sizeof(dir), "%s", target);
    dir[strlen(dir) - 1] = 0;                       // Strip the trailing '/'
    int n = tcSdCollectWavs(dir, &gScanNames[0][0], TC_MAX_SCAN_FILES, TC_PLAY_PATH);
    Serial.printf("[TRAIN] %s/ 找到 %d 個 WAV\n", dir, n);
    for (int i = 0; i < n; i++) {
      char full[sizeof(gLine) + TC_MAX_NAME_LEN + 2];
      snprintf(full, sizeof(full), "%s/%s", dir, gScanNames[i]);
      Serial.printf("  (%d/%d) %s\n", i + 1, n, full);
      addPath(full);
    }

  } else {
    // Several filenames separated by spaces
    char *p = target;
    while (*p) {
      while (*p == ' ') p++;
      if (!*p) break;
      char *start = p;
      while (*p && *p != ' ') p++;
      char saved = *p;
      *p = 0;
      addPath(start);
      *p = saved;
    }
  }

  Serial.printf("[TRAIN] 本次成功 %d 個，失敗 %d 個\n", okCount, failCount);
  gTrainSet.summary();
  showIdle();
}

// ---------------------------------------------------------------------------
// After sampling mode records a note: analyze -> name by pitch -> save -> into the bank -> into the training set
//
// The verdict uses the same rec_check as a single-note recording, so the two thresholds
// cannot disagree -- "fine while sampling, but clipped when the same note is recorded on
// its own" is the least believable kind of inconsistency.
static RecVerdict gSampVerdict = REC_OK;
static char       gSampReason[26] = "";
static char       gSampFix[26]    = "";

static void sampEvaluate(bool analysisOk) {
  RecCheck rc;
  rc.analysisOk  = analysisOk;
  rc.peak        = analyzerLastPeak();
  rc.clipRatio   = analyzerLastClipRatio();
  rc.noiseFloor  = analyzerLastNoiseFloor();
  rc.onsets      = analyzerLastOnsetCount();
  rc.noteDur     = analysisOk ? gProfile.noteDur : 0.0f;
  rc.f0          = analysisOk ? gProfile.f0      : 0.0f;
  rc.decayPerSec = analysisOk ? gProfile.sustainDecayPerSec : 0.0f;
  gSampVerdict = recCheckEval(rc, gSampReason, sizeof(gSampReason),
                                  gSampFix,    sizeof(gSampFix));
}

static void onSampleCaptured() {
  // Analysis and "piling into the training set" happen in the same pass, so remember the
  // original size first -- a failed verdict has to be able to roll back the entries just added.
  const int trainBefore = gTrainSet.size();

  if (!analyzeWavFile(TC_REC_PATH, gProfile, nullptr, &gTrainSet)) {
    sampEvaluate(false);
    gTrainSet.truncate(trainBefore);
    Serial.println(F("[SAMP] 這個音分析失敗（抓不到基頻？太短？），略過，繼續等下一個"));
    return;
  }
  sampEvaluate(true);

  // A failed verdict throws the whole thing away: not saved, not banked, not added to the
  // training set.
  //
  // This is the real damage done by "it keeps triggering when nobody is playing" -- the
  // triggering itself only wastes time; the real loss is that noise gets stored into
  // BANK.BIN as a timbre, affects every note from then on, and from the outside there is no
  // way to see which entry is the bad one. Better to miss a note than to take in a dirty one.
  if (gSampVerdict == REC_BAD) {
    gTrainSet.truncate(trainBefore);
    Serial.printf("[SAMP] 丟棄：%s（%s）—— 沒有存檔也沒有入庫\n",
                  gSampReason, gSampFix);
    return;
  }

  gModel.setProfile(&gProfile);

  int  midi = (int)lroundf(69.0f + 12.0f * log2f(gProfile.f0 / 440.0f));
  char nm[8];
  midiToName(midi, nm, sizeof(nm));
  float cents = 1200.0f * log2f(gProfile.f0 / tc_midiToHz((float)midi));

  char path[32];
  if (gSampDir[0]) snprintf(path, sizeof(path), "%s/%s.WAV", gSampDir, nm);
  else             snprintf(path, sizeof(path), "%s.WAV", nm);
  if (gSdOk) tcSdCopy(TC_REC_PATH, path);

  bool added = gBank.add(gProfile);
  if (gSdOk && added) bankSave(gBank, TC_BANK_PATH);
  gSampCount++;

  Serial.printf("[SAMP] 第 %d 個：%s（%.1f Hz，偏 %+.0f cent）-> %s  %s\n",
                gSampCount, nm, gProfile.f0, cents, path,
                added ? "已入庫" : "（庫已滿）");
  if (fabsf(cents) > 35.0f)
    Serial.println(F("       音準偏差偏大，確認一下樂器調音；音色庫是用音高配對的"));
  bankCoverage();
}

static void doTrain(int epochs) {
  if (gPlayer.playing()) gPlayer.stop();
  gKbd.allOff();

  displaySetState(TC_ST_TRAINING);
  displaySetProgress(0.0f);
  displayForce();

  Serial.println(F("[TRAIN] 訓練中，這段期間不能演奏（音訊中斷仍正常運作）..."));
  if (!trainMlp(gTrainSet, gTrainedW, epochs, TC_TRAIN_LR, (uint32_t)micros())) {
    displaySetState(TC_ST_ERROR, "train failed");
    displayForce();
    return;
  }

  gModel.adoptWeights(gTrainedW);
  if (gSdOk) gModel.saveWeights(gTrainedW, TC_MODEL_PATH);
  Serial.println(F("[TRAIN] 已套用。接著自動演奏並錄成 PLAY.WAV。"));
  showIdle();
}

// ============================================================== Play =====
// The chromatic scale has to cover "the range the timbre bank actually has, plus one octave
// above".
//
// The lower half is interpolation (the bank covers it); only the upper half really exercises
// transposition and formant correction -- one playback makes the difference between the two
// audible. It used to be hard-coded C3~B4, so with a bank holding only C4~B4 the first 12
// notes were all transposed downward, did not sound like the instrument, and were easily
// mistaken for a broken synth.
//
// One octave up and none down, because downward transposition is clearly worse in this
// architecture: going down has to conjure a fundamental lower than anything in the material,
// while going up is a rearrangement of harmonics that are already there.
static void applyScaleRangeFromBank() {
  if (gBank.n == 0) return;                  // With no timbre bank, keep the default C3~B4
  int lo = 127, hi = 0;
  for (int i = 0; i < gBank.n; i++) {
    const int m = (int)lroundf(69.0f + 12.0f * log2f(gBank.p[i].f0 / 440.0f));
    if (m < 0 || m > 127) continue;
    if (m < lo) lo = m;
    if (m > hi) hi = m;
  }
  if (lo > hi) return;
  scoreSetScaleRange(lo, hi + 12);
}

// Play. mode picks which score to play:
//
//   TC_SCORE_SCALE  chromatic scale -- for evaluation; evaluate.py can slice it note by note
//   TC_SCORE_CANON  Canon           -- for listening; three voices sounding together
//
// Setting mode explicitly every time (rather than "keep whatever it was") is deliberate:
// this is the only place the score is switched, and if one path forgets to set it, the next
// press of w records the Canon into PLAY.WAV -- and that file is the input to the evaluation
// baseline. A mistake like that produces no error message whatsoever.
static void startPlay(bool capture, ScoreMode mode) {
  // The range may have gained new samples since the last playback, so recompute it and
  // regenerate the score every time
  scoreSetMode(mode);
  applyScaleRangeFromBank();
  gPlayer.load();

  if (!gProfile.valid) {
    Serial.println(F("[PLAY] 還沒有音色，先按 r 或 a"));
    scoreSetMode(TC_SCORE_SCALE);       // This path does not play, so the mode must not be left on Canon
    return;
  }

  // State the sound source explicitly: every single note is produced by the additive
  // synthesizer, and not one of them is sample playback of the original recording.
  Serial.printf("[PLAY] 音源：加法合成器（最多 64 諧波 + 噪聲層），音色庫 %d 組\n", gBank.n);
  if (mode == TC_SCORE_CANON) {
    int lo, hi;
    scoreGetScaleRange(&lo, &hi);
    char a[8], b[8];
    midiToName(lo, a, sizeof(a));
    midiToName(hi, b, sizeof(b));
    // The Canon only shifts by whole octaves, so the key does not change; but which line
    // ended up where directly affects how it sounds. It does not fit on the panel, so at
    // least leave a record on the serial port, or "it sounds a chunk too low" is impossible
    // to investigate.
    Serial.printf("[PLAY] 卡農：D 大調三聲部，安置在 %s~%s（音色庫涵蓋範圍 + 一個八度）\n", a, b);
    Serial.printf("[PLAY] 八度安置：旋律 %+d、內聲部 %+d、低音 %+d 個半音\n",
                  scoreCanonShift(0), scoreCanonShift(1), scoreCanonShift(2));
    Serial.println(F("[PLAY] 這份譜是拿來聽的，不要拿它跑 evaluate.py —— 三聲部切不開"));
  } else {
    int lo, hi;
    scoreGetScaleRange(&lo, &hi);
    char a[8], b[8], c[8];
    midiToName(lo, a, sizeof(a));
    midiToName(hi, b, sizeof(b));
    midiToName(hi - 12, c, sizeof(c));
    Serial.printf("[PLAY] 音階 %s~%s 共 %d 音：%s~%s 是音色庫涵蓋的（內插），"
                  "%s 以上是往上移調\n", a, b, hi - lo + 1, a, c, c);
  }
  Serial.printf ("[PLAY] 音色來源：關鍵影格內插%s（MLP 權重 %.2f）\n",
                 gModel.mlpActive() ? " + MLP"
                                    : (gModel.hasMlp() ? "（MLP 權重 0，按 k 開啟）"
                                                       : "（無 MODEL.BIN）"),
                 gModel.blend());
  if (!gModel.hasMlp() && gSdOk && SD.exists(TC_MODEL_PATH))
    Serial.println(F("[PLAY] 注意：SD 上有 MODEL.BIN 但沒載入成功，按 m 重試"));

  const char *capPath = (mode == TC_SCORE_CANON) ? TC_CANON_PATH : TC_PLAY_PATH;
  gCapture = capture && gSdOk;
  if (capture && !gSdOk) Serial.println(F("[PLAY] SD 沒掛起來，這次不錄檔"));
  if (gCapture) gCap.start(capPath);

  gPlayer.start();

  refreshDisplayStatus();
  displaySetState(TC_ST_PLAYING);
  displaySetLine(0, gModel.mlpActive() ? "src additive + MLP" : "src additive + keyfr");
  displaySetLine(1, gCapture ? (mode == TC_SCORE_CANON ? "rec -> CANON.WAV"
                                                       : "rec -> PLAY.WAV") : "");
  displayForce();
}

static void stopPlay() {
  gPlayer.stop();
  if (gCap.active()) gCap.stop();
  gCapture = false;
  // Go back to the chromatic scale on stop, so the idle screen and the [SCORE] message do
  // not sit at "Canon in D" and make people think the next w will record the Canon
  scoreSetMode(TC_SCORE_SCALE);
  showIdle();
}

// ---------------------------------------------------------------------------
static void handleCommand(char *s) {
  while (*s == ' ') s++;
  if (!*s) return;
  // Commands are always lower-cased before dispatch. In an early version 's' (save) and 'S'
  // (sample) were two different functions; people naturally press lower case, and the result
  // was "pressed it, nothing happened" -- a trap that should not exist.
  char cmd = *s;
  if (cmd >= 'A' && cmd <= 'Z') cmd += 32;
  char *arg = s + 1;
  while (*arg == ' ') arg++;

  switch (cmd) {
    case 'r':
      if (!gSdOk) { Serial.println(F("[REC] SD 卡沒掛起來，錄音無處可存。按 d 重試掛載。")); break; }
      if (gPlayer.playing()) stopPlay();
      synth.allNotesOff();
      gAutoPlay = true;
      Serial.println(F("[REC] 3..2..1 開始，請演奏一個持續的單音"));
      displaySetState(TC_ST_RECORDING, "get ready...");
      displayForce();
      delay(600);
      if (!gRec.start()) {
        gAutoPlay = false;
        Serial.println(F("[REC] 啟動失敗（SD 寫入問題？）"));
        showIdle();
      }
      break;

    case 'a': doAnalyze(*arg ? arg : TC_REC_PATH); break;

    case 'n': doAddToTrainSet(arg); break;

    case 'z': {
      const int nSmp = gTrainSet.size();
      const int nBank = gBank.n;
      gTrainSet.clear();
      gBank.clear();
      gProfile.valid = false;            // The ADSR on the panel has to be invalidated along with it
      Serial.printf("[TRAIN] 已清空：訓練集 %d 筆、音色庫 %d 組\n", nSmp, nBank);
      Serial.println(F("       注意：SD 上的 BANK.BIN 沒有刪除，重開機會再載回來。"));
      char b0[26], b1[26];
      snprintf(b0, sizeof(b0), "trainset %d smp", nSmp);
      snprintf(b1, sizeof(b1), "bank     %d prof", nBank);
      showNotice("CLEARED", b0, b1, "SD files kept");
      break;
    }

    case 't': {
      int ep = *arg ? atoi(arg) : TC_TRAIN_EPOCHS;
      if (ep < 100 || ep > 60000) ep = TC_TRAIN_EPOCHS;
      doTrain(ep);
      if (gModel.hasMlp()) startPlay(true);      // Generate the finished output right after training
      break;
    }

    case 'o':
      if (gPlayer.playing()) stopPlay();
      synth.allNotesOff();
      gMonitor = true;
      gRec.beginMonitor();
      Serial.println(F("========== 輸入電平監看 =========="));
      sgtlDump();
      if (gMicInput)
        Serial.printf("  來源 麥克風(MIC)      micGain %d dB      觸發門檻 %.3f\n",
                      gMicGain, gRec.threshold());
      else
        Serial.printf("  來源 線性輸入(LINE IN) lineInLevel %d     觸發門檻 %.3f\n",
                      gLineLevel, gRec.threshold());
      Serial.println(F("  對著麥克風出聲，看下面的數字有沒有動。按任意鍵停止。"));
      Serial.println(F("  完全不動 = 麥克風沒接上/沒焊好，或輸入來源選錯（按 i 切換）"));
      Serial.println();
      Serial.println(F("  三頻段是用來檢查「錄音鏈有沒有把低頻吃掉」的。"));
      Serial.println(F("  乾淨素材的低頻佔比實測值（同樣是 C4）："));
      Serial.println(F("      鋼琴 50%   提琴 41%   長笛 27%   小號 17%"));
      Serial.println(F("  沒有通用門檻 —— 低頻佔比跟樂器種類高度相關，"));
      Serial.println(F("  但同一把樂器移動麥克風前後比較，數字會誠實反映差異。"));
      Serial.println(F("  （實測案例：麥克風錄的鋼琴只有 18%，參考素材是 50%）"));
      break;

    case 's':
      if (!gSdOk) { Serial.println(F("[SAMP] SD 沒掛起來，錄音無處可存")); break; }
      {                                            // s 0.01 -> use a lower threshold this time
        float t = *arg ? (float)atof(arg) : TC_TRIG_LEVEL;
        if (!(t > 0.0005f && t < 0.5f)) t = TC_TRIG_LEVEL;
        gSampThresh = t;
      }
      if (gPlayer.playing()) stopPlay();
      synth.allNotesOff();
      gSampling  = true;
      gSampCount = 0;
      // A new folder for each round. If it cannot be created, fall back to the root and
      // carry on as before -- one missing folder beats sampling mode not working at all.
      if (!tcSdMakeNextSet(gSampDir, sizeof(gSampDir))) gSampDir[0] = 0;
      gRec.armSession(gSampThresh);
      if (gSampDir[0])
        Serial.printf("[SAMP] 這一輪的音檔會存進 %s/，載入時用「n %s/」\n",
                      gSampDir, gSampDir);
      else
        Serial.println(F("[SAMP] 沒有建立資料夾，音檔會存在根目錄（可能蓋掉上一輪的）"));
      Serial.println(F("[SAMP] 開始吧：一次彈/拉一個音，錄完會自動命名存檔並入庫。"));
      Serial.println(F("       建議一個半音一個半音來，音與音之間停半秒。"));
      displaySetState(TC_ST_RECORDING, "SAMPLING - play a note");
      displayForce();
      break;

    // g adjusts the gain of "whichever input source is current", not always the mic.
    // The two sources have their own ranges and units; they share the command but not the value.
    case 'g': {
      if (gMicInput) {
        if (*arg) { gMicGain = tc_clampi(atoi(arg), 0, 63); applyInput(); }
        Serial.printf("[IN] micGain = %d dB（0~63；錄音峰值落在 0.3~0.8 最理想）\n", gMicGain);
      } else {
        if (*arg) { gLineLevel = tc_clampi(atoi(arg), 0, 15); applyInput(); }
        Serial.printf("[IN] lineInLevel = %d（0~15，數字越大越靈敏；"
                      "峰值一樣抓 0.3~0.8）\n", gLineLevel);
      }
      break;
    }

    case 'v': bankCoverage(); break;

    // Delete the audio files the program generated (REC.WAV / PLAY.WAV / note-name files).
    // Two letters 'yy' rather than a single key: this is irreversible, and a slip of the
    // finger should not be enough to fire it.
    // y y        -> delete the generated files in the root (REC/PLAY/note-name files)
    // y s        -> delete every sampling folder SETnn
    // y s SET02  -> delete only that one set
    case 'y':
      if (*arg == 'y') purgeGeneratedWavs();
      else if (*arg == 's') {
        const char *t = arg + 1;
        while (*t == ' ') t++;
        purgeSampleSets(*t ? t : "*");
      }
      else {
        Serial.println(F("[SD] 這會刪掉 REC.WAV、PLAY.WAV 以及採樣自動命名的音檔"));
        Serial.println(F("     （C4.WAV、Db4.WAV…）。你自己放的素材有前綴，不會被刪。"));
        Serial.println(F("     確定的話輸入「y y」。"));
        Serial.println(F("     要刪整組採樣資料夾（SETnn）請用「y s」，"
                         "只刪一組就「y s SET02」。"));
      }
      break;

    case 'd':
      if (!gSdOk) {
        Serial.println(F("[SD] 尚未掛載，重試中..."));
        gSdOk = tcSdBegin();
      }
      if (gSdOk) tcSdList();
      showIdle();
      break;

#if TC_HAS_MTP
    // Once it is off, loop() no longer services MTP and the drive on the PC stalls (it is
    // not ejected). This is for when you suspect MTP is interfering with SD writes: turn it
    // off, run again, and if DROP! disappears then that was it. The registered filesystems
    // are untouched, so turning it back on simply resumes -- no reboot needed.
    case 'u':
      gMtpOn = !gMtpOn;
      if (gMtpOn) {
        Serial.println(F("[MTP] 開 —— 電腦看得到 SD（演奏／錄音／分析期間仍會暫停服務）"));
      } else {
        Serial.println(F("[MTP] 關 —— 電腦端會停住不動，SD 寫入最安全"));
      }
      showIdle();
      break;
#endif

    case 'c':
      gDumpCsv = !gDumpCsv;
      Serial.printf("[ANA] CSV 輸出：%s\n", gDumpCsv ? "開" : "關");
      break;

    // p   -> chromatic scale (range follows the timbre bank, comparable note by note)
    // w   -> the same, but recorded to PLAY.WAV at the same time
    // j   -> Canon (three voices, for listening), recorded to CANON.WAV
    //        j n plays without recording -- no need to write 8 MB to the SD on every re-listen
    case 'p': startPlay(false); break;
    case 'w': startPlay(true);  break;
    case 'j': startPlay(!(*arg == 'n' || *arg == 'N'), TC_SCORE_CANON); break;
    case 'x': stopPlay();       break;


    case 'l':
      if (profileLoad(gProfile, TC_PROFILE_PATH)) {
        gModel.setProfile(&gProfile);
        profilePrint(gProfile);
      } else Serial.println(F("[PROFILE] 載入失敗"));
      showIdle();
      break;

    case 'm': gModel.loadWeights(); showIdle(); break;

    // What k toggles is the "blend weight", not "whether weights are loaded".
    //
    // The old version toggled _hasMlp, but TC_MLP_BLEND defaults to 0 -- both sides sounded
    // exactly the same, so the switch did nothing at all. What really decides whether the MLP
    // affects the sound is the blend weight, so that is what A/B has to toggle:
    // 0 (the measured keyframes alone) <-> TC_MLP_BLEND_AB.
    case 'k':
      if (!gModel.hasMlp() && !gModel.loadWeights()) {
        Serial.println(F("[MLP] 沒有 MODEL.BIN 可以比較，先按 t 訓練一個"));
        break;
      }
      if (gModel.blend() > 0.0f) {
        gModel.setBlend(0.0f);
        Serial.println(F("[MLP] 權重 0.00 -> 純關鍵影格（直接量到的分佈）"));
      } else {
        gModel.setBlend(TC_MLP_BLEND_AB);
        Serial.printf("[MLP] 權重 %.2f -> 關鍵影格 + MLP 修正\n", TC_MLP_BLEND_AB);
      }
      showIdle();
      break;

    // i switches the input source. Going through LINE IN (a 3.5mm cable straight into the
    // audio shield) bypasses the whole "speaker -> room -> mic" path -- measured, that path
    // loses 27~32 dB below 500 Hz, and in the recorded spectrum the piano ends up looking
    // less like a piano than a violin does. If there is a cable, use it.
    case 'i': gMicInput = !gMicInput; applyInput(); break;

    case '+':
      gVolume = tc_clampf(gVolume + 0.05f, 0.0f, 0.85f);
      sgtl.volume(gVolume);
      Serial.printf("[VOL] %.2f\n", gVolume);
      break;

    case '-':
      gVolume = tc_clampf(gVolume - 0.05f, 0.0f, 0.85f);
      sgtl.volume(gVolume);
      Serial.printf("[VOL] %.2f\n", gVolume);
      break;

    case '?': printHelp(); break;

    default:
      Serial.printf("不認得的指令 '%c'，按 ? 看說明\n", cmd);
      break;
  }
}

// ============================================================================
//  OLED menu tree
//
//  Each item carries a single "command string"; pressing it hands that string to
//  handleCommand() above. That way record/analyze/train each have exactly one
//  implementation and the serial port stays fully usable -- otherwise the same feature has
//  two sets of code, and changing one and forgetting the other is only a matter of time.
//
//  Only real operating flows belong here. CSV export, SD listings and the other debug aids
//  stay on the serial port; their output is meant to be read on a computer anyway, and
//  would be illegible on 128x64.
//
//  Labels are always in English: U8g2's small bitmap fonts have no CJK glyphs, so Chinese
//  comes out as a row of tofu. The serial port is unaffected and still prints full Chinese.
// ============================================================================
static int16_t gUiMicGain = 36;      // For editing in the menu; written back to gMicGain only on commit
static int16_t gUiEpochs  = 300;
// gUiOctave is declared in the upper half of the file -- showIdle() uses it before this point.
// Arduino only auto-inserts function prototypes, never variable declarations.

enum { PG_ROOT = 0, PG_PLAY, PG_SAMPLE, PG_TIMBRE, PG_TRAIN, PG_PURGE, PG_PURGESET };

static const UiItem kRootItems[] = {
  { "Play",     UI_PAGE, PG_PLAY,   nullptr, 0,0,0, nullptr },
  { "Sampling", UI_PAGE, PG_SAMPLE, nullptr, 0,0,0, nullptr },
  { "Timbre",   UI_PAGE, PG_TIMBRE, nullptr, 0,0,0, nullptr },
  { "Training", UI_PAGE, PG_TRAIN,  nullptr, 0,0,0, nullptr },
  // The menu normally owns the screen; this item steps aside so the original status panel
  // can be seen (Back brings it back)
  { "Status",   UI_CMD,  0, "?stat", 0,0,0, nullptr },
};

static const UiItem kPlayItems[] = {
  { "Keyboard 12 keys",UI_CMD,    0, "?keys",  0,0,0, nullptr },
  { "PC keyboard map", UI_CMD,    0, "?pckb",  0,0,0, nullptr },
  { "Scale",           UI_CMD,    0, "p",      0,0,0, nullptr },
  { "Scale + record",  UI_CMD,    0, "w",      0,0,0, nullptr },
  // The Canon is the only "for listening" item, so it goes after the chromatic scale:
  // measure first, listen second, is the real order of work
  { "Canon",           UI_CMD,    0, "j n",    0,0,0, nullptr },
  { "Canon + record",  UI_CMD,    0, "j",      0,0,0, nullptr },
  { "Stop",            UI_CMD,    0, "x",      0,0,0, nullptr },
  { "Key octave",      UI_ADJUST, 0, "?oct %d", -2, 2, 1, &gUiOctave },
  { "Volume +",        UI_CMD,    0, "+",      0,0,0, nullptr },
  { "Volume -",        UI_CMD,    0, "-",      0,0,0, nullptr },
};

static const UiItem kSampleItems[] = {
  { "Record 1 note", UI_CMD,    0, "r", 0,0,0, nullptr },
  { "Auto sampling", UI_CMD,    0, "s", 0,0,0, nullptr },
  { "Input monitor", UI_CMD,    0, "o", 0,0,0, nullptr },
  // The upper limit is MIC's 0~63. On a switch to LINE IN, handleCommand clamps to 0~15 by
  // itself, so turning it too far does no harm -- the last few steps simply do nothing. The
  // two sources have different ranges, and a menu item's limits are compile-time constants;
  // there is no room in them for "it depends".
  { "Input gain",    UI_ADJUST, 0, "g %d", 0, 63, 1, &gUiMicGain },
  { "Mic / Line in", UI_CMD,    0, "i", 0,0,0, nullptr },
};

static const UiItem kTimbreItems[] = {
  { "Load all WAV",  UI_CMD, 0, "n *", 0,0,0, nullptr },
  // Continuous sampling writes into SETnn/, while Load all WAV only scans the root -- with
  // no computer in the loop, there was no way to get them back after a round of sampling.
  // This item is that way.
  { "Load newest SET", UI_CMD, 0, "n SET", 0,0,0, nullptr },
  { "Analyze REC",   UI_CMD, 0, "a",   0,0,0, nullptr },
  { "Bank coverage", UI_CMD, 0, "v",   0,0,0, nullptr },
  { "Reload profile",UI_CMD, 0, "l",   0,0,0, nullptr },
  { "Clear trainset",UI_CMD, 0, "z",   0,0,0, nullptr },
  { "Delete WAV files", UI_PAGE, PG_PURGE,  nullptr, 0,0,0, nullptr },
  { "Delete SET dirs",  UI_PAGE, PG_PURGESET, nullptr, 0,0,0, nullptr },
};

// Deleting files is irreversible, so it gets a confirmation page of its own.
// Putting "Cancel" first is deliberate: you arrive with the cursor on item 0, so a slipped
// double-press of OK only cancels, it never deletes anything. The existing menu machinery
// is enough for this; there is no need to add a new state for it.
static const UiItem kPurgeItems[] = {
  { "Cancel",          UI_CMD, 0, "?back", 0,0,0, nullptr },
  { "DELETE rec+synth",UI_CMD, 0, "y y",   0,0,0, nullptr },
};

// Sampling folders get a separate confirmation page. Cancel goes first here too.
//
// Deliberately no "list SET01/SET02 and let you pick": the menu's item table is a
// compile-time constant, and listing dynamically would mean rebuilding it at run time --
// not worth that much extra state just to delete files. To delete one particular set, type
// "y s SET02" on the serial port -- anyone who wants to pick and choose is at a computer anyway.
static const UiItem kPurgeSetItems[] = {
  { "Cancel",          UI_CMD, 0, "?back", 0,0,0, nullptr },
  { "DELETE all sets", UI_CMD, 0, "y s",   0,0,0, nullptr },
};

static const UiItem kTrainItems[] = {
  { "Epochs",        UI_ADJUST, 0, "t %d", 50, 2000, 50, &gUiEpochs },
  { "MLP on/off",    UI_CMD,    0, "k", 0,0,0, nullptr },
  { "Reload MODEL",  UI_CMD,    0, "m", 0,0,0, nullptr },
};

// The item count comes from sizeof, not from a hand-written number.
// Writing it by hand went wrong once already: add an item, forget the number, and the menu
// is one row short (write too many and it reads past the end). That mistake produces no
// compiler warning at all; the only way to catch it is to stare at the OLED.
#define UI_PAGE_DEF(title, arr, parent) \
  { title, arr, (uint8_t)(sizeof(arr) / sizeof((arr)[0])), parent }

static const UiPage kPages[] = {
  UI_PAGE_DEF("TimbreClone", kRootItems,   PG_ROOT),
  UI_PAGE_DEF("Play",        kPlayItems,   PG_ROOT),
  UI_PAGE_DEF("Sampling",    kSampleItems, PG_ROOT),
  UI_PAGE_DEF("Timbre",      kTimbreItems, PG_ROOT),
  UI_PAGE_DEF("Training",    kTrainItems,  PG_ROOT),
  UI_PAGE_DEF("Delete files?", kPurgeItems,    PG_TIMBRE),
  UI_PAGE_DEF("Delete SETnn?",  kPurgeSetItems, PG_TIMBRE),
};

// Hand the menu's current state to display. ui.cpp never touches u8g2; the strings are put
// together here and then sent over.
static void uiRender() {
  // Two cases where the menu has to give way:
  //
  //   1) gUi.suspended()            an explicit request (Status, key mode)
  //   2) a status panel owns it     sampling, monitoring, recording, analyzing, training, playing
  //
  // Case 2 was added later, and it is the real reason Auto sampling "did nothing at all when
  // pressed": displaySetState() already pushes the menu aside when it enters a busy state,
  // but once the command has run, uiHandleKey() calls in here unconditionally and draws the
  // menu straight back -- the panel exists for too short a time to see, and the level meter
  // never appears either (displayService sees menuOn and draws only the menu).
  //
  // The clue that cracked it was "the screen is actually still updating": that is not a
  // crash, that is drawing the wrong thing.
  if (gUi.suspended() || displayState() != TC_ST_IDLE) {
    displaySetMenu(nullptr, nullptr, 0, 0, 0, 0, false);
    return;
  }
  char rows[UI_VISIBLE_ROWS][26];
  const int total = gUi.rowCount();
  const int first = gUi.topRow();
  int n = 0;
  for (int i = 0; i < UI_VISIBLE_ROWS && (first + i) < total; i++) {
    gUi.rowText((uint8_t)(first + i), rows[i], sizeof(rows[i]));
    n++;
  }
  displaySetMenu(gUi.title(), rows, n, gUi.cursor(), first, total, gUi.editing());
  displayForce();
}

static void uiRenderFwd() { uiRender(); }

static void pcKeyboardHelp() {
  Serial.println();
  Serial.println(F("========== USB 電腦鍵盤鍵位 =========="));
  Serial.println(F("  上排（高八度）    2 3   5 6 7      <- 黑鍵"));
  Serial.println(F("                   Q W E R T Y U    <- 白鍵  C5~B5"));
  Serial.println(F("  下排（原位）      S D   G H J      <- 黑鍵"));
  Serial.println(F("                   Z X C V B N M    <- 白鍵  C4~B4"));
  Serial.println(F("  左/右方向鍵  換八度      空白鍵  全部停音"));
  Serial.println(F("  上/下方向鍵  選單移動    Enter 確定   Esc/Backspace 返回"));
  Serial.println(F("  同時按的音數受鍵盤硬體限制：一般鍵盤最多 6 個，"));
  Serial.println(F("  沒有防鬼鍵設計的薄膜鍵盤可能 3 個就開始漏。"));
  Serial.printf ("  目前狀態：%s\n", gKbd.connected() ? "已連接" : "沒有偵測到電腦鍵盤");
  Serial.println(F("======================================"));
}

static void uiHandleKey(UiKey k) {
  // The 5 seconds of a one-shot recording (r) ignore the menu; it ends on its own.
  //
  // The reason is that the audio queue is being written to the SD during that time, and
  // drawing the menu means a whole round of OLED I2C traffic -- the two compete for time and
  // drop samples. Sampling mode (gSampling) is exempt: it has to be abortable with a key
  // press, and that abort takes the path below, which draws no menu.
  if (gRec.active() && !gSampling) return;

  // Notice screen: any key dismisses it early, no need to wait it out.
  if (gNoticeUntil) { gNoticeUntil = 0; noticeDismissed(); return; }

  // Error screen: any key returns to idle.
  //
  // Without this the screen wedges -- uiRender() draws no menu when the state is not IDLE,
  // while the menu state machine is still in the foreground, so up/down only move a cursor
  // nobody can see. This is the very same class of bug as Auto sampling's "pressed it,
  // nothing happened": add a state and you have to remember to add its exit.
  if (displayState() == TC_ST_ERROR) { showIdle(); return; }

  // Key mode: only Back does anything (leave); the other 12 keys are left to sound notes.
  // Up/down deliberately do nothing here -- a stray press transposing mid-performance would
  // be very hard to track down.
  if (gKeys.enabled()) {
    if (k == UI_KEY_BACK) {
      gKeys.setEnabled(false);
      gUi.setSuspended(false);
      showIdle();
    }
    return;
  }

  // These modes eat the keys themselves (any key ends sampling, any key stops monitoring),
  // which matches the serial port's behavior; do not let the menu interfere.
  // setSuspended(false) has to come before showIdle(): showIdle() redraws internally, and in
  // the other order that redraw still believes the menu is standing aside, so it stops at the
  // idle panel and the menu only comes back after another button press -- which again looks
  // like "nothing happened".
  if (gSampling)  { endSampSession();
                    gUi.setSuspended(false); showIdle(); return; }
  if (gMonitor)   { gRec.endMonitor(); gMonitor = false;
                    Serial.println(F("[MON] 停止監看"));
                    gUi.setSuspended(false); showIdle(); return; }

  const char *cmd = gUi.feed(k);
  // Anything starting with '?' is menu-internal, not a serial command; intercept it and handle it here
  if (cmd && strcmp(cmd, "?back") == 0) {     // Cancel on a confirmation page
    uiHandleKey(UI_KEY_BACK);
    return;
  }
  if (cmd && strcmp(cmd, "?stat") == 0) {
    gUi.setSuspended(true);
    showIdle();
    return;
  }
  if (cmd && strcmp(cmd, "?keys") == 0) {
    if (gPlayer.playing()) gPlayer.stop();
    gKeys.setTranspose((int8_t)(gUiOctave * 12));
    gKbd.setTranspose((int8_t)(gUiOctave * 12));
    gKeys.setEnabled(true);
    gUi.setSuspended(true);                 // Give the screen over to the key-mode panel
    displaySetState(TC_ST_PLAYING, "KEYBOARD  C4-B4");
    return;
  }
  if (cmd && strcmp(cmd, "?pckb") == 0) {
    pcKeyboardHelp();
    gUi.setSuspended(true);
    displaySetState(TC_ST_PLAYING, "PC KEYBOARD");
    displaySetLine(0, "ZSXDCVGBHNJM = C4-B4");
    displaySetLine(1, "Q2W3ER5T6Y7U = C5-B5");
    displaySetLine(2, "arrows: octave / menu");
    displaySetLine(3, gKbd.connected() ? "status: connected"
                                       : "status: not detected");
    displayForce();
    return;
  }
  if (cmd && strncmp(cmd, "?oct ", 5) == 0) {
    // Both kinds of keys share one octave setting, otherwise the pitch would jump when you
    // switch input method
    gKeys.setTranspose((int8_t)(gUiOctave * 12));
    gKbd.setTranspose((int8_t)(gUiOctave * 12));
    Serial.printf("[KEYS] 移調 %+d 個八度\n", (int)gUiOctave);
    return;
  }
  if (cmd && cmd[0]) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", cmd);
    Serial.printf("[UI] %s\n", buf);
    gMicGain = gUiMicGain;                    // Sync the values of the value items back first
    handleCommand(buf);

    // A result notice has just come up (clear succeeded, and so on): do not draw the menu
    // back over it.
    //
    // The notice deliberately uses the TC_ST_IDLE state, so that when it times out the
    // automatic showIdle() slides seamlessly back into the menu. But that also means it
    // cannot escape the displayState() test below, and needs a check of its own here.
    // The menu is not suspended here -- it has to come back by itself once the notice is
    // gone, without the user pressing Back.
    if (gNoticeUntil) return;

    // The command may have handed the screen to a status panel (sampling, monitoring,
    // recording, playing...). In that case let the menu retreat to the background and be
    // called back with Back -- otherwise the menu state machine is still in the foreground
    // and up/down move a cursor nobody can see.
    if (displayState() != TC_ST_IDLE) { gUi.setSuspended(true); return; }
  }
  uiRender();
}

// The menu has two sources: the 4 physical buttons on the panel, and the arrow keys of the
// USB PC keyboard. Both go through the same uiHandleKey(), so their behavior is necessarily
// identical -- no "this option can be reached with the buttons but not with the keyboard"
// difference that only turns up on the hardware.
//
// The queue is drained in a loop: typing on a PC keyboard is fast, and handling one key per
// loop makes repeated presses noticeably laggy.
static void uiTick() {
  for (int guard = 0; guard < 8; guard++) {
    UiKey k = gButtons.poll();
    if (k == UI_KEY_NONE) k = gKbd.popUiKey();
    if (k == UI_KEY_NONE) return;
    uiHandleKey(k);
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2500) { }

  // MTP has to come up early: the host starts asking questions right after enumeration, and
  // if we wait until the SD and the OLED are initialized before answering, Windows may
  // already have decided the device is not responding.
  // The filesystem is registered later, once the SD is mounted; keeping begin() and
  // addFilesystem() apart is fine.
#if TC_HAS_MTP
  MTP.begin();
#endif

  gButtons.begin();
  gUi.begin(kPages, sizeof(kPages) / sizeof(kPages[0]));

  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("  TimbreClone  -  即時樂器音色複製 (Teensy 4.1)"));
  Serial.println(F("=============================================="));

  displayBegin();

  // Audio memory: 2 synth channels + 2 reverb + the mic queue + the stereo capture queue.
  // The capture queue backs up temporarily during SD writes, so this is more generous than before.
  AudioMemory(120);

  sgtl.enable();
  sgtl.volume(gVolume);

  applyInput();       // It deals with the ADC high-pass filter inside (see the notes on applyInput)

  verb.roomsize(0.42f);
  verb.damping(0.55f);
  mixL.gain(0, 0.88f);  mixL.gain(1, 0.13f);
  mixR.gain(0, 0.88f);  mixR.gain(1, 0.13f);

  gModel.setBank(&gBank);
  synth.setModel(&gModel);
  synth.setMasterGain(0.18f);
  synth.setVibrato(50.0f, 4.8f);   // Upper limit and default rate; the actual depth/frequency are decided by the profile measurements

  gRec.begin(&recQueue);
  gCap.begin(&capQL, &capQR);
  gPlayer.begin(&synth);
  gKeys.begin(&synth);       // 12 physical piano keys (wiring described in keys.h)
  gKbd.begin(&synth);        // USB PC keyboard as piano keys (key map described in kbd_in.h)
  gKbd.usbBegin();           // USB Host startup (used to live in midi_in, now moved to kbd_in)

  analyzerSetProgressCallback(onAnalyzeProgress);
  trainerSetProgressCallback(onTrainProgress);

  gSdOk = tcSdBegin();
  if (gSdOk) {
    if (profileLoad(gProfile, TC_PROFILE_PATH)) {
      gModel.setProfile(&gProfile);
      profilePrint(gProfile);
    } else {
      Serial.println(F("[PROFILE] 尚無音色指紋"));
      gProfile.valid = false;
    }
    gModel.loadWeights(TC_MODEL_PATH);
    if (!bankLoad(gBank, TC_BANK_PATH)) Serial.println(F("[BANK] 尚無音色庫"));
    tcSdList();
  } else {
    gProfile.valid = false;
  }

#if TC_HAS_MTP
  if (gSdOk) {
    MTP.addFilesystem(SD, "TimbreClone SD");
    Serial.printf("[MTP] SD 已掛成電腦上的磁碟機（目前%s）。按 u 可切換。\n",
                  gMtpOn ? "開啟" : "關閉");
    Serial.println(F("      注意：演奏／錄音／分析期間不會服務 MTP，電腦端會看起來卡住，"));
    Serial.println(F("      那是刻意的 —— 那幾段正在對同一張卡做即時寫入。"));
  } else {
    Serial.println(F("[MTP] SD 沒掛起來，沒有東西可以分享給電腦"));
  }
#endif

  printHelp();
  Serial.println(F("提示：按 r 錄一個單音，或把 WAV 丟進 SD 後輸入「n *」批次載入再按 t 訓練。"));
  Serial.println(F("      USB 電腦鍵盤插上 USB Host 埠就能直接彈目前的音色，不用下任何指令。"));

  // With no timbre loaded, every note request is silently dropped. Say so plainly, so nobody thinks the hardware is broken.
  if (!synth.hasTimbre()) {
    Serial.println();
    Serial.println(F("  ***  目前沒有載入任何音色  ***"));
    Serial.println(F("  在這個狀態下，按 p 演奏或彈鍵盤都不會有聲音（不是故障）。"));
    Serial.println(F("  先做這件事：把單音 WAV 放進 SD 根目錄，然後輸入「n *」再按 t。"));
    Serial.println();
  }
  showIdle();
}

// ---------------------------------------------------------------------------
void loop() {
  // ---- USB PC keyboard ----
  // First thing in the loop: USB Host enumeration and packet reception both happen here, and
  // the latency decides how it feels to play.
  // Note that analysis/training block, and the keyboard is dead for that time (allOff() is
  // called before going in).
  gKbd.usbService();

  // ---- Physical keys / buttons / OLED menu ----
  //
  // Must be first in loop(). There are several `return`s below (one each for monitoring,
  // sampling and recording), and if key servicing sits after them, the buttons are completely
  // dead in those modes.
  //
  // This is exactly why "the buttons did nothing after pressing Auto sampling": the screen was
  // still updating, so it looked like a crash, when in fact the keys were never being read and
  // only the serial port could get you out.
  // With it first, adding a few more returns later cannot repeat the mistake.
  gKeys.service();
  uiTick();

  // ---- Auto-dismiss of the result notice ----
  // After key servicing: if that key press has already dismissed the notice, this will not do it twice.
  if (gNoticeUntil && (int32_t)(millis() - gNoticeUntil) >= 0) {
    gNoticeUntil = 0;
    noticeDismissed();
  }

  // ---- Serial port ----
  while (Serial.available()) {
    char c = Serial.read();
    if (gMonitor && c != '\n' && c != '\r') {
      gRec.endMonitor();
      gMonitor = false;
      gLineLen = 0;
      Serial.println(F("[MON] 停止監看"));
      showIdle();
      continue;
    }
    // In sampling mode any key means "stop" -- with an instrument in your hands you should
    // not have to type out a whole command
    if (gSampling && c != '\n' && c != '\r') {
      gLineLen = 0;
      endSampSession();
      showIdle();
      continue;
    }
    if (c == '\n' || c == '\r') {
      gLine[gLineLen] = 0;
      if (gLineLen) handleCommand(gLine);
      gLineLen = 0;
    } else if (gLineLen < sizeof(gLine) - 1) {
      gLine[gLineLen++] = c;
    }
  }

  // ---- Input level monitor ----
  if (gMonitor) {
    gRec.serviceMonitor();
    static uint32_t lastMon = 0;
    if (millis() - lastMon > 250) {
      lastMon = millis();
      float rms = gRec.monRms(), pk = gRec.monPeak();
      int bars = (int)(tc_clampf(rms / 0.5f, 0.f, 1.f) * 30);
      char meter[34];
      for (int i = 0; i < 30; i++) meter[i] = (i < bars) ? '#' : '.';
      meter[30] = 0;
      const float dc = gRec.monDc(), ac = gRec.monAc();
      Serial.printf("[MON] %s RMS %6.4f (%5.1f dB)  峰值保持 %6.4f  %s\n",
                    meter, rms, 20.0f * log10f(rms + 1e-9f), pk,
                    ac >= gRec.threshold() ? "<- 足以觸發" : "<- 還不到門檻");
      // Look at DC and AC separately. A fixed DC offset makes the RMS above look like "there
      // is signal", but it is not sound -- and it lifts the whole ambient-adaptive trigger
      // threshold.
      Serial.printf("      直流 %+7.4f   交流 RMS %6.4f (%5.1f dB)%s\n",
                    dc, ac, 20.0f * log10f(ac + 1e-9f),
                    fabsf(dc) > 3.0f * (ac + 1e-6f) && fabsf(dc) > 0.005f
                      ? "   <- 直流蓋過交流，這不是收音問題" : "");

      // Three bands. The overall level can look fine while all the energy is up high, and the
      // RMS alone shows nothing of it -- measured, a piano recorded through the mic is exactly
      // like that: the fundamental is all but absent.
      const float lo = gRec.monLow(), md = gRec.monMid(), hi = gRec.monHigh();
      const float tot = lo + md + hi + 1e-9f;
      Serial.printf("      頻段  低<300Hz %4.0f%%   中300~2k %4.0f%%   高>2k %4.0f%%\n",
                    100.0f * lo / tot, 100.0f * md / tot, 100.0f * hi / tot);

      char b[26];
      displaySetState(TC_ST_RECORDING, "INPUT MONITOR");
      snprintf(b, sizeof(b), "RMS %.4f  pk %.3f", rms, pk);
      displaySetLine(0, b);
      snprintf(b, sizeof(b), "L%3.0f%% M%3.0f%% H%3.0f%%",
               100.0f * lo / tot, 100.0f * md / tot, 100.0f * hi / tot);
      displaySetLine(1, b);
      // Show the three bands as bars, so the proportions are obvious at a glance
      {
        char bar[22];
        const int nL = (int)(20.0f * lo / tot + 0.5f);
        const int nM = (int)(20.0f * md / tot + 0.5f);
        int k = 0;
        for (int i = 0; i < nL && k < 20; i++) bar[k++] = 'L';
        for (int i = 0; i < nM && k < 20; i++) bar[k++] = 'M';
        while (k < 20) bar[k++] = 'H';
        bar[20] = 0;
        displaySetLine(2, bar);
      }
      // Deliberately no automatic verdict. The low-band share is highly instrument-dependent --
      // measured on clean material: piano 50%, violin 41%, flute 27%, trumpet 17%.
      // A badly recorded piano comes in at 18%, exactly like a normal trumpet, so any fixed
      // threshold would produce false alarms. All this does is show the numbers; the judgement
      // is left to the person who knows what they are playing.
      displaySetLine(3, "target peak 0.3-0.8");
      displayForce();
    }
    return;
  }

  // ---- Continuous sampling mode ----
  if (gSampling) {
    if (gRec.service()) {                  // A note has just been recorded
      onSampleCaptured();
      displaySetState(TC_ST_RECORDING, "SAMPLING - next note");
      char b[26];
      snprintf(b, sizeof(b), "captured %d", gSampCount);
      displaySetLine(0, b);
      // Someone holding an instrument is not watching the serial port; the verdict has to
      // appear on the panel.
      //
      // Recording quality comes before "change instrument": a bad take should never have gone
      // into the bank in the first place, and the user can fix it right away (adjust the gain,
      // record again), so it deserves to be seen first.
      // A failed take is "thrown away", not "accepted but a bit off" --
       // those two mean completely different things to the user, and the panel has to tell them apart.
      snprintf(b, sizeof(b), "%s pk%.2f hr%.0fdB",
               gSampVerdict == REC_OK   ? "ok"
             : gSampVerdict == REC_WARN ? "warn" : "DROPPED",
               analyzerLastPeak(), gRec.headroomDb());
      displaySetLine(1, b);
      if (gSampVerdict != REC_OK) {
        displaySetLine(2, gSampReason);
        displaySetLine(3, gSampFix);
      } else if (gBank.lastAddSuspect) {
        snprintf(b, sizeof(b), "!! diff timbre %.1f", gBank.lastAddDist);
        displaySetLine(2, b);
        displaySetLine(3, "changed instrument?");
      } else {
        displaySetLine(2, gSampReason);
        displaySetLine(3, gSampFix);
      }
      displayForce();
    } else {
      static uint32_t lastUi = 0;
      if (millis() - lastUi > 150) {
        lastUi = millis();
        char b[26];
        if (gRec.active()) {
          displaySetProgress(gRec.progress());
          displaySetLine(0, "recording...");
        } else if (gRec.calibrating()) {
          // Nothing triggers during these 600 ms. Without saying so, the user assumes it is broken.
          displaySetProgress(-1.0f);
          displaySetLine(0, "listening to room...");
        } else {
          displaySetProgress(-1.0f);
          int bars = (int)(tc_clampf(gRec.level() / 0.5f, 0.f, 1.f) * 16);
          char meter[20];
          for (int i = 0; i < 16; i++) meter[i] = (i < bars) ? '#' : '.';
          meter[16] = 0;
          snprintf(b, sizeof(b), "%s", meter);
          displaySetLine(0, b);
        }
        snprintf(b, sizeof(b), "captured %d  %s %d", gSampCount,
                 gMicInput ? "mic" : "line", gMicInput ? gMicGain : gLineLevel);
        displaySetLine(1, b);
        // The threshold is automatic, so it must show what it is right now and how loud the
        // ambient noise is -- otherwise both "why does it never trigger" and "why does it
        // trigger all the time" turn into complaints with nothing to investigate.
        snprintf(b, sizeof(b), "amb%.3f thr%.3f", gRec.ambient(), gRec.threshold());
        displaySetLine(2, b);
        if (!gRec.calibrating() && gRec.ambient() >= gRec.baseThresh())
          displaySetLine(3, "room too noisy");
        else if (gRec.headroomDb() > 0.0f && gRec.headroomDb() < TC_TRIG_MIN_HEADROOM_DB)
          displaySetLine(3, "low margin - play up");
        else
          displaySetLine(3, "");
        displayService();

        // People with no OLED need to see the level too, or "nothing happens" leaves no clue at all
        static uint32_t lastSer = 0;
        if (!gRec.active() && millis() - lastSer > 1500) {
          lastSer = millis();
          Serial.printf("[SAMP] 等待中… 電平 %.4f / 門檻 %.4f（環境 %.4f）%s\n",
                        gRec.level(), gRec.threshold(), gRec.ambient(),
                        gRec.level() < gRec.threshold() * 0.2f
                          ? "（幾乎沒訊號，按 o 檢查麥克風）" : "");
        }
      }
    }
    return;                                // Nothing else happens while sampling
  }

  // ---- Mic recording (top priority, must not be held up by SD/OLED) ----
  if (gRec.active()) {
    if (gRec.service()) {
      char buf[2] = {'a', 0};
      handleCommand(buf);                  // Analyze as soon as the recording finishes
    } else {
      static uint32_t lastRecUi = 0;
      if (millis() - lastRecUi > 300) {    // Only a very low-rate screen update while recording
        lastRecUi = millis();
        char b[26];
        displaySetState(TC_ST_RECORDING, "sing / play one note");
        displaySetProgress(gRec.progress());
        snprintf(b, sizeof(b), "peak %.2f", gRec.peak());
        displaySetLine(0, b);
        displayForce();
      }
    }
    return;
  }

  // ---- Playback scheduling + synchronized capture ----
  //
  // "Give the screen back when playback ends" used to live inside gCap.active(), i.e. only the
  // "play and record" path ever restored it. The auto-play after pressing r does not enable
  // capture, so once it finished nothing rescued the screen and it stayed on the previous
  // state forever.
  // Now "was playing a moment ago, is not playing now" is tracked on its own, independent of
  // whether anything is being recorded.
  {
    static bool wasPlaying = false;
    gPlayer.service();
    if (gCap.active()) {
      gCap.service();
      if (!gPlayer.playing()) {            // Playback finished, close the file out
        gCap.stop();
        gCapture = false;
        Serial.printf("[PLAY] 成品已存成 %s（內容 100%% 由加法合成產生）\n",
                      scoreGetMode() == TC_SCORE_CANON ? TC_CANON_PATH : TC_PLAY_PATH);
        // The plan here was to send MTP.send_DeviceResetEvent() to tell the host the files had
        // changed. Not possible: MTP_Teensy wraps every send_*Event() in #if USE_EVENTS == 1,
        // and that macro is off inside the library's own translation unit. Name lookup then
        // lands on the same-named declaration in the private section, and the compile error is
        // "is private within this context" -- not "no such member", so at first glance it looks
        // like nothing more than an access-permission problem.
        //
        // #define USE_EVENTS 1 in the .ino does not help: Arduino compiles libraries separately,
        // and sketch macros never reach them. Turning it on means patching the library itself,
        // which would tie this project to a hand-modified MTP_Teensy and stop it building on
        // another machine -- not worth it.
        //
        // The practical impact is small: MTP is not serviced during playback anyway (see the
        // condition at the end of loop()), so the host cannot have cached a half-written size.
        // Hitting F5 in the file manager shows the correct file. The MTP section of the README
        // records this.
      }
    }
    const bool nowPlaying = gPlayer.playing();
    if (wasPlaying && !nowPlaying) {
      // Reaching the end naturally also has to switch back to the chromatic scale. Doing it
      // only in stopPlay() is not enough -- a Canon played through to the end never passes
      // there, the mode stays at CANON, and what the next w records no longer matches the
      // filename.
      scoreSetMode(TC_SCORE_SCALE);
      showIdle();
    }
    wasPlaying = nowPlaying;
  }

  // Key-mode panel: shows which notes are held down
  if (gKeys.enabled()) {
    static uint32_t lastKeyUi = 0;
    if (millis() - lastKeyUi > TC_OLED_REFRESH_MS) {
      lastKeyUi = millis();
      char b[26];
      keysDownText(b, sizeof(b));
      displaySetLine(0, b);
      snprintf(b, sizeof(b), "keys %d  voices %d", gKeys.downCount(), synth.activeVoices());
      displaySetLine(1, b);
      snprintf(b, sizeof(b), "octave %+d   BACK=exit", (int)gUiOctave);
      displaySetLine(2, b);
    }
  }

  // ---- Display: live playing ----
  // While someone is playing, show the key count and the CPU; one second after release, back
  // to the idle screen.
  {
    static bool wasLive = false;
    const bool live = (synth.activeVoices() > 0) && !gPlayer.playing();
    if (live) {
      static uint32_t lastLive = 0;
      if (millis() - lastLive > TC_OLED_REFRESH_MS) {
        lastLive = millis();
        char b[26];
        const bool viaPc = gKbd.downCount() > 0;
        displaySetState(TC_ST_PLAYING, viaPc ? "LIVE - PC keyboard"
                                             : "LIVE - keys");
        displaySetProgress(-1.0f);
        if (viaPc) {
          gKbd.downText(b, sizeof(b));
          displaySetLine(0, b);
        } else {
          snprintf(b, sizeof(b), "voices %d", synth.activeVoices());
          displaySetLine(0, b);
        }
        snprintf(b, sizeof(b), "cpu %2.0f%%  notes %lu", AudioProcessorUsage(),
                 (unsigned long)gKbd.noteCount());
        displaySetLine(1, b);
      }
      wasLive = true;
    } else if (wasLive) {
      wasLive = false;
      showIdle();
    }
  }

  // ---- Display ----
  if (gPlayer.playing()) {
    static uint32_t lastUi = 0;
    if (millis() - lastUi > TC_OLED_REFRESH_MS) {
      lastUi = millis();
      char b[26];
      displaySetProgress(gPlayer.progress());
      snprintf(b, sizeof(b), "voices %d  cpu %2.0f%%", synth.activeVoices(),
               AudioProcessorUsage());
      displaySetLine(2, b);
      if (gCap.active()) {
        snprintf(b, sizeof(b), "rec %4.1fs%s", gCap.seconds(),
                 gCap.dropped() ? "  DROP!" : "");
        displaySetLine(1, b);
      }
    }
  }
  displayService();

  // ---- Print resource usage every 2 s ----
  static uint32_t lastStat = 0;
  if (gPlayer.playing() && millis() - lastStat > 2000) {
    lastStat = millis();
    Serial.printf("[STAT] 進度 %3.0f%%  複音 %d  CPU %4.1f%% (峰值 %4.1f%%)  blocks %d\n",
                  gPlayer.progress() * 100.0f, synth.activeVoices(),
                  AudioProcessorUsage(), AudioProcessorUsageMax(),
                  AudioMemoryUsageMax());
    AudioProcessorUsageMaxReset();
  }

  // ---- USB MTP ----
  //
  // The position matters: this has to be the last thing in loop(), and it needs the condition
  // below.
  //
  // Monitoring, sampling and mic recording each return above, so getting here means we are not
  // in any of those modes; analysis and training are blocking calls, and loop() is not running
  // at all while they go. The only thing that reaches here while also touching the SD is "play
  // and capture" -- 44.1 kHz stereo written in real time, which is exactly the scenario
  // MTP_Teensy issue #41 says will time out and fail, so it is excluded explicitly.
  //
  // The price is that the drive on the PC stops responding for those stretches. That is a
  // deliberate trade: better a spinning cursor in the file manager than a dropped block left
  // in the finished output as an audible glitch.
#if TC_HAS_MTP
  if (gMtpOn && !gPlayer.playing() && !gCap.active()) MTP.loop();
#endif
}

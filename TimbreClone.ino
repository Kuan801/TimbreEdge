// ============================================================================
//  TimbreClone  -  Teensy 4.1 + Audio Shield (SGTL5000) + 128x64 OLED
//
//  聽一個單音（SD 上的 WAV 或麥克風錄 5 秒）→ 萃取音色指紋 →
//  用加法合成 + 小型 MLP 重建這個樂器 → 演奏《卡農》副歌四聲部。
//  MLP 可以在電腦上訓練，也可以直接在 Teensy 上訓練。
//
//  ── 序列埠指令 (115200，Line ending 要選 New Line) ────────────────────
//    r            麥克風錄 5 秒 -> REC.WAV，錄完自動分析
//    a [檔名]     分析 WAV（不給檔名 = REC.WAV）
//    d            列出 SD 根目錄
//
//    n [目標]     分析並加入訓練集。目標可以是：
//                   (空白)          REC.WAV
//                   A.WAV           單一檔案
//                   A.WAV B.WAV ..  多個檔案
//                   *               根目錄所有 WAV
//                   SAMPLES/        該資料夾所有 WAV
//    t [epochs]   在 Teensy 上訓練 MLP，完成後存 MODEL.BIN 並套用
//    z            清空訓練集
//
//    p            演奏卡農副歌（全部由加法合成產生）
//    w            演奏並同步錄成 PLAY.WAV
//    x            停止
//
//    s            連續採樣模式（自動觸發錄音）
//    l            重新載入 PROFILE.BIN
//    m            重新載入 MODEL.BIN
//    k            A/B：MLP 混合權重在 0 與 TC_MLP_BLEND_AB 之間切換
//    i            切換輸入來源（麥克風 / 線性輸入）
//    u            開／關 USB MTP（把 SD 掛成電腦的磁碟機；需要對應的 USB Type）
//    c            切換是否輸出 FRAMES.CSV
//    +  -         主音量
//    ?            說明
//    電腦鍵盤      插 USB Host 埠。ZSXDCVGBHNJM = C4~B4，
//                  Q2W3ER5T6Y7U = C5~B5，方向鍵換八度／操作選單（見 kbd_in.h）
//
//  ── 接線 ───────────────────────────────────────────────────────────────
//    Audio Shield  疊在 Teensy 4.1 上
//    OLED (I2C)    SDA = 18,  SCL = 19,  VCC = 3.3V,  GND = GND
//                  （與 SGTL5000 共用 I2C，位址不衝突：OLED 0x3C / codec 0x0A）
//    按鈕（可選）  D2 = 錄音並分析      D3 = 演奏 / 停止
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

// USB MTP：把 SD 掛成電腦上的磁碟機。條件編譯的理由見 config.h 的 TC_HAS_MTP。
#if TC_HAS_MTP
#include <MTP_Teensy.h>
static bool gMtpOn = TC_MTP_DEFAULT_ON;
#endif

// ---------------------------------------------------------------- 音訊圖 --
//   合成器 -> reverb -> 混音器 -> 耳機
//                              \-> 擷取佇列（錄成 PLAY.WAV）
//   錄下來的就是聽到的那份訊號，全部來自加法合成，沒有任何取樣播放。
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

// ------------------------------------------------------------------ 模組 --
InstrumentProfile gProfile;
DMAMEM ProfileBank gBank;      // 多音高音色庫（16 組約 75 KB）
TimbreModel       gModel;
Recorder          gRec;
StereoCapture     gCap;
Player            gPlayer;
TrainSet          gTrainSet;

static void uiRenderFwd();          // 定義在選單樹那一段（讓 showIdle 先用得到）
static void uiHandleKey(UiKey k);   // 確認頁的 Cancel 要能自己送一個返回鍵
// doAnalyze() 錄完自動演奏時要用它，但它定義在後面。
// Arduino 會自動補函式原型，g++（tools/sim 的 inocheck）不會 —— 補一行給它。
// 預設值只能寫在這一份宣告上（C++ 不准同一個作用域重複給預設值），
// 所以下面的定義不再寫 = TC_SCORE_SCALE。
static void startPlay(bool capture, ScoreMode mode = TC_SCORE_SCALE);

// 琴鍵模式的移調，以八度為單位。選單的「Key octave」項目直接編輯它。
//
// 放在這裡而不是跟其他 gUi* 一起放在選單樹那一段，是因為 showIdle() 要用它顯示
// 目前八度，而 showIdle() 在檔案上半部。Arduino 的預處理只會自動補「函式原型」，
// 不會補變數宣告 —— 變數放在使用點後面就是 'not declared in this scope'。
static int16_t gUiOctave = 0;

DMAMEM MlpWeights gTrainedW;                                   // 訓練產出
DMAMEM char       gScanNames[TC_MAX_SCAN_FILES][TC_MAX_NAME_LEN];  // 批次掃描

bool  gDumpCsv  = false;
bool  gAutoPlay = false;      // 錄音→分析→自動演奏
bool  gCapture  = false;      // 這次演奏要不要錄成 PLAY.WAV
float gVolume   = 0.55f;
bool  gMicInput = true;
bool  gSdOk     = false;
bool  gSampling = false;      // 連續採樣模式
int   gSampCount = 0;         // 這次 session 收了幾個音
// 這一輪採樣的資料夾，例如 "SET03"。空字串代表沒開成功，退回根目錄。
char  gSampDir[12] = "";
// 最近一輪「有錄到東西」的採樣資料夾。gSampDir 在 session 結束時會清掉，
// 但「載入剛剛採的那一組」需要在結束之後還記得它，所以另外留一份。
char  gLastSet[12] = "";
int   gMicGain  = 36;         // MIC 模式的前級增益 0~63 dB
// LINE IN 模式的輸入靈敏度 0~15（SGTL5000 的定義：數字越大 = 滿刻度電壓越小
// = 越靈敏）。以前寫死 5（約 1.33 Vp-p），那是給訊號源等級的裝置用的；
// 手機／筆電的耳機孔輸出小得多，寫死就只能錄到很小的電平。
// 現在跟 micGain 一樣可以調 —— 走 LINE IN 是繞開喇叭與房間最乾淨的一條路，
// 那條路要好用，增益就得調得動。
int   gLineLevel = 5;
bool  gMonitor  = false;      // 輸入電平監看
float gSampThresh = TC_TRIG_LEVEL;

char    gLine[96];
uint8_t gLineLen = 0;

// ====================================================== OLED 進度回呼 =====
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
    // f0 -> 音名，面板上比 Hz 好讀
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
  // 第 4 行顯示 USB Host 埠上的電腦鍵盤在不在線上。
  if (gKbd.connected()) snprintf(b, sizeof(b), "PC kbd ready  oct%+d", (int)gUiOctave);
  else                  snprintf(b, sizeof(b), "USB: no keyboard");
  displaySetLine(3, b);
  displayForce();
  uiRenderFwd();     // 內容已備妥；畫面要給選單還是狀態，由它決定
}

// ---------------------------------------------------------------------------
//  短暫的結果通知
//
//  「按了之後有沒有成功」如果只印在序列埠，站在裝置前面的人完全看不到。
//  清空音色庫、刪檔這種不可逆的操作尤其需要回饋 —— 沒有回饋的話，
//  使用者會不確定到底有沒有生效，然後再按一次。
//
//  設計成「顯示幾秒後自動回到閒置」，而不是「按鍵才消失」：
//  成功訊息不該要求使用者再操作一次。但按任意鍵可以提早關掉。
//
//  錯誤畫面（例如多次起音）刻意不用這個機制 —— 那個要停在畫面上等人看到。
// ---------------------------------------------------------------------------
static uint32_t gNoticeUntil = 0;

// 通知消失（自動或被按掉）之後要做什麼。
//
// 錄音判定面板是唯一有後續動作的：合格的話接著自動演奏一次，讓人馬上聽到
// 合成結果。以前是分析完立刻演奏，判定面板根本來不及被看到 ——
// 所以改成「先讓人看完，再演奏」，而不是把演奏拿掉。
static void noticeDismissed() {
  if (gAutoPlay) { gAutoPlay = false; startPlay(false); return; }
  showIdle();
}

static void showNotice(const char *title, const char *l0, const char *l1,
                       const char *l2, uint32_t ms = 2500) {
  displaySetState(TC_ST_IDLE, title);      // 用 IDLE 才不會擋住選單的恢復
  displaySetProgress(-1.0f);
  displaySetLine(0, l0 ? l0 : "");
  displaySetLine(1, l1 ? l1 : "");
  displaySetLine(2, l2 ? l2 : "");
  displaySetLine(3, "");
  displaySetMenu(nullptr, nullptr, 0, 0, 0, 0, false);   // 暫時把選單推開
  displayForce();
  gNoticeUntil = millis() + ms;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  編解碼器自我檢查：把 SGTL5000 的暫存器讀回來
//
//  為什麼需要：inputSelect() / micGain() 都會回傳 bool，但那只證明
//  「I2C 寫出去了」。麥克風完全沒訊號的時候真正想知道的是「晶片現在是什麼
//  狀態」—— 輸入選的是 MIC 還是 LINE IN、增益到底多少、ADC 有沒有被靜音、
//  偏壓有沒有供上。這幾個暫存器可以一次把問題分成兩邊：
//
//      讀不到 / 值不對  -> 軟體設定沒生效，或 I2C 根本不通（查 SDA18/SCL19）
//      值都對           -> 設定沒問題，問題在類比端（麥克風、焊點、接線）
//
//  沒有這個的話只能一直猜，而兩邊的處理方式完全不同。
//
//  SGTL5000 在 Audio Shield 上的 I2C 位址是 0x0A；暫存器位址 16 bit 大端，
//  資料也是 16 bit。位元定義取自 PJRC 的 control_sgtl5000.cpp。
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
  if (Wire.endTransmission(false) != 0) return false;      // 重複起始，不放開匯流排
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
  // 這幾個都會回傳「I2C 有沒有寫成功」。以前直接丟掉回傳值 ——
  // 匯流排不通的時候完全沒有跡象，只會表現成「麥克風收不到聲音」。
  const bool okSel  = sgtl.inputSelect(gMicInput ? AUDIO_INPUT_MIC : AUDIO_INPUT_LINEIN);
  const bool okGain = gMicInput ? sgtl.micGain(gMicGain) : sgtl.lineInLevel(gLineLevel);
  if (!okSel || !okGain)
    Serial.println(F("[CODEC] ! 設定寫不進 SGTL5000（I2C 失敗）。按 o 看完整自我檢查。"));

  // ---- ADC 高通濾波器：讓它量好直流偏移，然後凍結 -------------------------
  //
  // 這裡曾經是 adcHighPassFilterDisable()，理由是「濾波器會動到最低頻那一段，
  // 而整套音色分析建立在諧波振幅的相對關係上」。動機沒錯，但選錯了函式：
  // Disable 對應的是 ADC_HPF_BYPASS，PJRC 的註解寫得很清楚 ——
  // 「Bypassed and offset not updated」，也就是**直流完全不扣**。
  //
  // 實測後果：MIC 路徑的直流偏移直接變成一塊固定的墊子。監看上看到
  // RMS 0.026、96~97% 的能量在 300 Hz 以下、而且不管出不出聲都不動；
  // 觸發門檻是「環境噪音 × 邊界」算出來的，於是被這塊墊子抬高，
  // 症狀表現成「麥克風很不靈敏 / 採樣不觸發」。分析端也一樣中招：
  // analyzer 沒有做去均值，直流會直接汙染包絡與 FFT 的最低幾格。
  //
  // 正確的做法是 Freeze（ADC_HPF_FREEZE）：
  //   「Freeze the ADC high-pass filter offset register.
  //     The offset continues to be subtracted from the ADC data stream.」
  // 直流照扣，但濾波器不再持續調整 —— 原本擔心的「濾波器動到訊號」不會發生。
  //
  // 順序很重要：偏移量跟增益有關，所以每次改輸入或增益都要重新量一次。
  // 先讓它跑著把偏移量收斂，再凍結。
  sgtl.adcHighPassFilterEnable();
  delay(400);                       // 讓偏移量收斂（只在改設定時發生）
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

// 刪掉程式自己產生的音檔。音色資料（BANK/PROFILE/MODEL）不動 ——
// 那是另一件事，要清音色請用 Clear trainset。
static void purgeGeneratedWavs() {
  if (!gSdOk) {
    Serial.println(F("[SD] 卡片沒掛起來，沒東西可刪。按 d 重試掛載。"));
    showNotice("NO SD CARD", "cannot delete", "press d to remount", "");
    return;
  }

  // 先掃一遍收集檔名，再刪。邊迭代邊刪目錄項目在 FAT 上行為未定義，
  // 實務上會漏掉一半的檔案。
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

static void bankCoverage();   // 定義在下面，endSampSession() 先用到

// ---------------------------------------------------------------------------
//  採樣收尾。序列埠與選單兩條路都會走到這裡。
//
//  原本兩邊各寫一份，差異是序列埠那邊多了 gTrainSet.summary()。
//  這種「兩份幾乎一樣的收尾」遲早會分岔 —— 現在多了「把 BANK.BIN 複製一份
//  進資料夾」這件事，更不該有兩個版本。
static void endSampSession() {
  gRec.endSession();
  gSampling = false;

  Serial.printf("[SAMP] 這次共採到 %d 個音\n", gSampCount);
  bankCoverage();
  gTrainSet.summary();

  if (gSdOk && gBank.n > 0) {
    bankSave(gBank, TC_BANK_PATH);
    // 資料夾裡也留一份，這樣一組素材就是自足的：音檔 + 那組音檔算出來的音色庫。
    // 之後想換回這一組，複製回根目錄就好，不必重跑一次分析。
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
//  刪除採樣資料夾
//
//  arg 是 "*" 就全刪，是 "SET02" 就只刪那一個。
//  音色資料（根目錄的 BANK/PROFILE/MODEL）不動 —— 那是另一件事。
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

// 音色庫的音域覆蓋狀況：哪些半音有了、哪些還缺
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

// ====================================================== 分析 / 訓練集 =====
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

  // 分析是阻塞的，鍵盤在這段期間沒人服務。先把壓著的音收掉，
  // 否則跑完出來會有一堆卡住的音還在響。
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
  // 每分析一個檔就存進音色庫，之後每個音符會挑音高最接近的那組來用
  if (gBank.add(gProfile)) { gBank.summary(); if (gSdOk) bankSave(gBank, TC_BANK_PATH); }
  if (gDumpCsv) Serial.println(F("[ANA] 已輸出 FRAMES.CSV"));
  if (addToTrainSet) Serial.printf("[TRAIN] 加入 %d 筆樣本\n", gTrainSet.size() - before);
  return true;
}

// ---------------------------------------------------------------------------
//  單音錄音的成功判定面板
//
//  以前分析完只有「多次起音」會停在畫面上，其餘一律直接跳走。結果是：錄壞了
//  （削波、太小聲、觸發被噪音搶先）完全沒有跡象，要等到合成出來覺得不像，
//  才回頭一個一個猜。實際查那兩份手機喇叭錄的 REC.WAV 就是這樣浪費掉的。
//
//  現在不管成功失敗都停一下，把四個數字跟一句「該怎麼改」放上去。
//  判定邏輯在 rec_check.cpp（純數值，桌機有測試）。
//
//    第 1 行  音名 / 基頻 / 音長
//    第 2 行  峰值 / 訊噪比 / 衰減
//    第 3 行  結論
//    第 4 行  下一步怎麼做
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
  // 判定不合格就用 ERROR 狀態：它會停在畫面上等人按鍵，不會自己消失。
  // 合格的用通知，看幾秒就好，不要擋住下一步。
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

  // 訊噪比量不到的時候顯示 "--"，不要印一個很漂亮的數字 ——
  // 那會讓人以為錄音很乾淨，但其實只是前面沒有可以當底噪的那一段。
  if (recCheckSnrKnown(rc.noiseFloor))
    snprintf(b, sizeof(b), "pk%.2f snr%2.0f dec%.2f",
             rc.peak, recCheckSnrDb(rc.noiseFloor), rc.decayPerSec);
  else
    snprintf(b, sizeof(b), "pk%.2f snr-- dec%.2f", rc.peak, rc.decayPerSec);
  displaySetLine(1, b);
  displaySetLine(2, reason);
  displaySetLine(3, fix);

  // 序列埠給完整版，面板受限於 21 個字元只能給結論
  Serial.printf("[CHK] %s：%s / %s\n", recVerdictTitle(v), reason, fix);
  Serial.printf("      峰值 %.3f  削波 %.3f%%  訊噪比 ", rc.peak, rc.clipRatio * 100.0f);
  if (recCheckSnrKnown(rc.noiseFloor)) Serial.printf("%.0f dB", recCheckSnrDb(rc.noiseFloor));
  else                                 Serial.print(F("量不到（起音在錄音的最開頭）"));
  Serial.printf("  起音 %d 次  音長 %.2f 秒  衰減 %.2f/秒\n",
                rc.onsets, rc.noteDur, rc.decayPerSec);

  if (v == REC_BAD) {
    displaySetMenu(nullptr, nullptr, 0, 0, 0, 0, false);
    displayForce();
    gNoticeUntil = 0;              // 不自動消失，等人按鍵
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

  // 不合格就不要自動演奏 —— 那會蓋掉剛剛畫出來的判定，而判定正是重點。
  if (v == REC_BAD) { gAutoPlay = false; return; }

  // 合格的話，判定面板顯示 4 秒之後才自動演奏（見 loop() 裡的 gNoticeUntil）。
  //
  // 演奏要走 startPlay() 而不是直接 gPlayer.start()：後者會跳過三件事 ——
  // 把畫面切成 PLAYING、依音色庫重算音階音域、載入樂譜。畫面沒切的後果最嚴重，
  // 狀態停在 ANALYZING，而 uiRender() 看到非 IDLE 就不畫選單，
  // 於是按鈕明明有反應卻什麼都不顯示，看起來就是當機。
}

// n 指令：支援單檔、多檔、萬用字元、整個資料夾
// 「n SET」= 載入最近那一輪採樣的資料夾。刻意只認沒有數字也沒有斜線的
// 純 "SET"：真正的資料夾一定是 SETnn（tcSdMakeNextSet 固定補兩位數字），
// 所以不會跟真實檔名或資料夾撞名。
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
    // 根目錄全掃
    int n = tcSdCollectWavs("/", &gScanNames[0][0], TC_MAX_SCAN_FILES, TC_PLAY_PATH);
    Serial.printf("[TRAIN] 根目錄找到 %d 個 WAV\n", n);
    for (int i = 0; i < n; i++) {
      Serial.printf("  (%d/%d) %s\n", i + 1, n, gScanNames[i]);
      addPath(gScanNames[i]);
    }
    // 連續採樣是存進 SETnn/ 的，不在根目錄。只掃到 REC.WAV 一個檔卻剛採完
    // 一整輪的人會以為是壞掉了 —— 這裡直接把資料夾列出來。
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
    // 「最近那一組」。選單沒辦法讓人打字，這條是它唯一能走的路。
    //
    // 「最近」的定義：這次開機採過的話就是那一組（gLastSet）；否則退回
    // 編號最大的。不用編號當唯一依據，是因為 tcSdMakeNextSet 會把使用者
    // 刪掉的號碼補回去 —— 刪了 SET02 再採一輪，新的那組叫 SET02，
    // 編號最大的 SET03 反而是舊的。
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
    // 指定資料夾全掃
    char dir[sizeof(gLine)];
    snprintf(dir, sizeof(dir), "%s", target);
    dir[strlen(dir) - 1] = 0;                       // 去掉尾巴的 '/'
    int n = tcSdCollectWavs(dir, &gScanNames[0][0], TC_MAX_SCAN_FILES, TC_PLAY_PATH);
    Serial.printf("[TRAIN] %s/ 找到 %d 個 WAV\n", dir, n);
    for (int i = 0; i < n; i++) {
      char full[sizeof(gLine) + TC_MAX_NAME_LEN + 2];
      snprintf(full, sizeof(full), "%s/%s", dir, gScanNames[i]);
      Serial.printf("  (%d/%d) %s\n", i + 1, n, full);
      addPath(full);
    }

  } else {
    // 以空白分隔的多個檔名
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
// 採樣模式錄完一個音之後：分析 -> 依音高命名 -> 另存 -> 進音色庫 -> 進訓練集
//
// 判定用跟單音錄音同一套 rec_check，兩邊的門檻才不會各說各話 ——
// 「採樣時說沒問題、單獨錄同一個音卻說削波」是最難相信的那種不一致。
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
  // 分析跟「堆進訓練集」是同一趟做的，所以先記住原本的大小 ——
  // 判定不合格的話要能把剛加進去的那幾筆退回來。
  const int trainBefore = gTrainSet.size();

  if (!analyzeWavFile(TC_REC_PATH, gProfile, nullptr, &gTrainSet)) {
    sampEvaluate(false);
    gTrainSet.truncate(trainBefore);
    Serial.println(F("[SAMP] 這個音分析失敗（抓不到基頻？太短？），略過，繼續等下一個"));
    return;
  }
  sampEvaluate(true);

  // 判定不合格就整筆丟掉：不存檔、不入庫、不進訓練集。
  //
  // 這是「沒有人在演奏卻一直觸發」造成的實際傷害 —— 觸發本身只是浪費時間，
  // 真正的損失是那些噪音被當成音色存進 BANK.BIN，之後每個音都會受影響，
  // 而且從外面看不出來是哪一筆有問題。寧可漏掉一個音，也不要收一筆髒的。
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

// ============================================================== 演奏 =====
// 半音階要涵蓋「音色庫實際有的音域，再往上一個八度」。
//
// 下半段是內插（音色庫涵蓋得到），上半段才是真正在考驗移調與共振峰校正 ——
// 一次演奏就能聽出兩者的差別。以前寫死 C3~B4，音色庫只有 C4~B4 時
// 前 12 個音全是往下移調，聽起來不像，卻很容易被誤會成合成器壞掉。
//
// 只往上一個八度不往下，是因為往下移調在這套架構裡明顯比較差：
// 往下要憑空生出比素材更低的基頻，往上則是既有諧波的重新配置。
static void applyScaleRangeFromBank() {
  if (gBank.n == 0) return;                  // 沒有音色庫就維持預設 C3~B4
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

// 演奏。mode 決定要彈哪一份譜：
//
//   TC_SCORE_SCALE  半音階 —— 評測用，逐音可以被 evaluate.py 切開比對
//   TC_SCORE_CANON  卡農   —— 試聽用，三聲部同時發聲
//
// 每次都明確設定 mode（而不是「沿用上一次」）是刻意的：這裡是唯一會切換
// 樂譜的地方，只要有一條路徑忘了設，下一次按 w 就會把卡農錄進 PLAY.WAV，
// 而那個檔是評測基準的輸入 —— 這種錯不會有任何錯誤訊息。
static void startPlay(bool capture, ScoreMode mode) {
  // 音域可能在上一次演奏之後又採了新音，所以每次都重算並重新產生樂譜
  scoreSetMode(mode);
  applyScaleRangeFromBank();
  gPlayer.load();

  if (!gProfile.valid) {
    Serial.println(F("[PLAY] 還沒有音色，先按 r 或 a"));
    scoreSetMode(TC_SCORE_SCALE);       // 這條路沒有演奏，模式不能留在卡農
    return;
  }

  // 明確宣告音源：每一個音符都由加法合成器產生，
  // 沒有任何一個是原始錄音的取樣播放。
  Serial.printf("[PLAY] 音源：加法合成器（最多 64 諧波 + 噪聲層），音色庫 %d 組\n", gBank.n);
  if (mode == TC_SCORE_CANON) {
    int lo, hi;
    scoreGetScaleRange(&lo, &hi);
    char a[8], b[8];
    midiToName(lo, a, sizeof(a));
    midiToName(hi, b, sizeof(b));
    // 卡農只移整個八度，所以調性不變；但哪一條線被搬到哪裡會直接影響聽感，
    // 面板上塞不下，至少要在序列埠留下紀錄，否則「聽起來低了一截」查不起。
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
  // 停下來就回到半音階：閒置畫面與 [SCORE] 訊息才不會停在「Canon in D」，
  // 讓人以為下一次按 w 錄到的是卡農
  scoreSetMode(TC_SCORE_SCALE);
  showIdle();
}

// ---------------------------------------------------------------------------
static void handleCommand(char *s) {
  while (*s == ' ') s++;
  if (!*s) return;
  // 指令一律轉小寫再分派。早期版本 's'(存檔) 和 'S'(採樣) 是兩個不同功能，
  // 使用者很自然會按小寫，結果就是「按了沒反應」—— 這種陷阱不該存在。
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
      gProfile.valid = false;            // 面板上的 ADSR 也要跟著失效
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
      if (gModel.hasMlp()) startPlay(true);      // 訓練完直接生成成品
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
      {                                            // s 0.01 -> 這次用較低的門檻
        float t = *arg ? (float)atof(arg) : TC_TRIG_LEVEL;
        if (!(t > 0.0005f && t < 0.5f)) t = TC_TRIG_LEVEL;
        gSampThresh = t;
      }
      if (gPlayer.playing()) stopPlay();
      synth.allNotesOff();
      gSampling  = true;
      gSampCount = 0;
      // 每一輪開一個新資料夾。開不出來就退回根目錄照舊運作 ——
      // 少一個資料夾比整個採樣模式不能用好。
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

    // g 調的是「目前這個輸入來源」的增益，不是固定調麥克風。
    // 兩個來源各有各的範圍與單位，共用一個指令但不共用數值。
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

    // 刪掉程式自己產生的音檔（REC.WAV / PLAY.WAV / 音名檔）。
    // 用兩個字母 'yy' 而不是單一鍵：這是不可逆的，不該手滑就觸發。
    // y y        -> 刪根目錄的產生檔（REC/PLAY/音名檔）
    // y s        -> 刪全部採樣資料夾 SETnn
    // y s SET02  -> 只刪那一組
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
    // 關掉之後 loop() 就不再服務 MTP，電腦端的磁碟機會停住（不是拔除）。
    // 這是給「懷疑 SD 寫入被 MTP 干擾」時用的：關掉再跑一次，如果 DROP! 消失
    // 就確定是它。註冊過的檔案系統不動，開回來就繼續用，不用重開機。
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

    // p   -> 半音階（音域跟著音色庫走，逐音可比對）
    // w   -> 同上，但同時錄成 PLAY.WAV
    // j   -> 卡農（三聲部，拿來聽的），錄成 CANON.WAV
    //        j n 只演奏不錄檔 —— 反覆試聽時不需要每次都寫 8 MB 進 SD
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

    // k 切換的是「混合權重」而不是「有沒有載入權重」。
    //
    // 舊版切 _hasMlp，但預設 TC_MLP_BLEND 是 0 —— 兩邊聽起來完全一樣，
    // 這個開關等於沒有作用。真正決定 MLP 有沒有影響聲音的是混合權重，
    // 所以 A/B 要切它：0（純量測到的關鍵影格）<-> TC_MLP_BLEND_AB。
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

    // i 切換輸入來源。走 LINE IN（3.5mm 線直接進 audio shield）可以整條
    // 繞開「喇叭 -> 房間 -> 麥克風」—— 實測那條路在 500 Hz 以下衰減 27~32 dB，
    // 錄下來的鋼琴頻譜上比小提琴還不像鋼琴。有線就用線。
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
//  OLED 選單樹
//
//  每個項目只帶一個「指令字串」，按下去就交給上面的 handleCommand() 執行。
//  這樣錄音/分析/訓練那些流程只有一份實作，序列埠也維持完全可用 ——
//  否則同一個功能兩套程式碼，改了一邊忘了另一邊是遲早的事。
//
//  只放實際操作流程。CSV 匯出、SD 列檔這些除錯用的留在序列埠，
//  它們的輸出本來就是給人在電腦上讀的，搬到 128x64 也看不清楚。
//
//  標籤一律用英文：U8g2 的小點陣字型沒有中文字符，塞中文會變成一排豆腐。
//  序列埠不受影響，那邊仍然是完整的中文訊息。
// ============================================================================
static int16_t gUiMicGain = 36;      // 選單編輯用，送出時才寫回 gMicGain
static int16_t gUiEpochs  = 300;
// gUiOctave 宣告在檔案上半部 —— showIdle() 在這裡之前就用到它了。
// Arduino 只會自動補函式原型，不會補變數宣告。

enum { PG_ROOT = 0, PG_PLAY, PG_SAMPLE, PG_TIMBRE, PG_TRAIN, PG_PURGE, PG_PURGESET };

static const UiItem kRootItems[] = {
  { "Play",     UI_PAGE, PG_PLAY,   nullptr, 0,0,0, nullptr },
  { "Sampling", UI_PAGE, PG_SAMPLE, nullptr, 0,0,0, nullptr },
  { "Timbre",   UI_PAGE, PG_TIMBRE, nullptr, 0,0,0, nullptr },
  { "Training", UI_PAGE, PG_TRAIN,  nullptr, 0,0,0, nullptr },
  // 選單平常佔著畫面，這一項把它讓開去看原本的狀態面板（按返回回來）
  { "Status",   UI_CMD,  0, "?stat", 0,0,0, nullptr },
};

static const UiItem kPlayItems[] = {
  { "Keyboard 12 keys",UI_CMD,    0, "?keys",  0,0,0, nullptr },
  { "PC keyboard map", UI_CMD,    0, "?pckb",  0,0,0, nullptr },
  { "Scale",           UI_CMD,    0, "p",      0,0,0, nullptr },
  { "Scale + record",  UI_CMD,    0, "w",      0,0,0, nullptr },
  // 卡農是唯一「拿來聽」的項目，放在半音階後面：先量再聽是實際的操作順序
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
  // 上限用 MIC 的 0~63。切到 LINE IN 時 handleCommand 會自己夾到 0~15，
  // 所以轉過頭不會出事，只是後面幾格沒作用 —— 兩個來源的範圍不同，
  // 而選單項的上下限是編譯期常數，塞不進「看情況」的邏輯。
  { "Input gain",    UI_ADJUST, 0, "g %d", 0, 63, 1, &gUiMicGain },
  { "Mic / Line in", UI_CMD,    0, "i", 0,0,0, nullptr },
};

static const UiItem kTimbreItems[] = {
  { "Load all WAV",  UI_CMD, 0, "n *", 0,0,0, nullptr },
  // 連續採樣存進 SETnn/，而 Load all WAV 只掃根目錄 —— 不接電腦的流程
  // 採完之後就沒有路把它們載回來。這一項就是那條路。
  { "Load newest SET", UI_CMD, 0, "n SET", 0,0,0, nullptr },
  { "Analyze REC",   UI_CMD, 0, "a",   0,0,0, nullptr },
  { "Bank coverage", UI_CMD, 0, "v",   0,0,0, nullptr },
  { "Reload profile",UI_CMD, 0, "l",   0,0,0, nullptr },
  { "Clear trainset",UI_CMD, 0, "z",   0,0,0, nullptr },
  { "Delete WAV files", UI_PAGE, PG_PURGE,  nullptr, 0,0,0, nullptr },
  { "Delete SET dirs",  UI_PAGE, PG_PURGESET, nullptr, 0,0,0, nullptr },
};

// 刪檔是不可逆的，所以獨立一頁做確認。
// 「Cancel」放第一項是刻意的：進來游標停在第 0 項，手滑連按兩下確定
// 只會取消，不會刪東西。用現成的選單機制就做得到，不必為它加新的狀態。
static const UiItem kPurgeItems[] = {
  { "Cancel",          UI_CMD, 0, "?back", 0,0,0, nullptr },
  { "DELETE rec+synth",UI_CMD, 0, "y y",   0,0,0, nullptr },
};

// 採樣資料夾另外一頁確認。同樣把 Cancel 放第一項。
//
// 這裡刻意不做「逐一列出 SET01/SET02 讓你挑」：選單的項目表是編譯期常數，
// 要動態列出得整個改成執行期組裝，為了刪檔加那麼多狀態不划算。
// 想刪特定一組，序列埠打「y s SET02」就好 —— 會想精挑細選的人本來就在電腦前面。
static const UiItem kPurgeSetItems[] = {
  { "Cancel",          UI_CMD, 0, "?back", 0,0,0, nullptr },
  { "DELETE all sets", UI_CMD, 0, "y s",   0,0,0, nullptr },
};

static const UiItem kTrainItems[] = {
  { "Epochs",        UI_ADJUST, 0, "t %d", 50, 2000, 50, &gUiEpochs },
  { "MLP on/off",    UI_CMD,    0, "k", 0,0,0, nullptr },
  { "Reload MODEL",  UI_CMD,    0, "m", 0,0,0, nullptr },
};

// 項目數由 sizeof 算，不用手寫。
// 手寫過一次就出過事：加了項目忘了改數字，選單會少一列（多寫則是讀到界外）。
// 這種錯不會有任何編譯警告，只能靠在 OLED 上盯著看才發現。
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

// 把選單目前的樣子交給 display。ui.cpp 不碰 u8g2，字串在這裡組好再送過去。
static void uiRender() {
  // 選單要讓位的兩種情況：
  //
  //   1) gUi.suspended()          明確要求讓位（Status、琴鍵模式）
  //   2) 畫面正被狀態面板佔著     採樣、監看、錄音、分析、訓練、演奏
  //
  // 第 2 點是後來補的，而且是 Auto sampling「按了完全沒反應」的真正原因：
  // displaySetState() 進入忙碌狀態時本來就會把選單推開，但指令執行完之後
  // uiHandleKey() 會無條件呼叫這裡，又把選單畫回去 —— 面板存在的時間短到
  // 看不見，電平表也永遠出不來（displayService 看到 menuOn 就只畫選單）。
  //
  // 查這條線索的關鍵是「螢幕其實一直在更新」：那就不是當機，是畫錯東西。
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
  // 單次錄音（r）的那 5 秒不理選單，它自己會結束。
  //
  // 理由是那段期間音訊佇列正在往 SD 寫，而畫選單要跑一整輪 OLED 的 I2C 傳輸，
  // 兩者會搶時間、掉取樣。採樣模式（gSampling）不在此限 —— 它本來就得能被
  // 按鍵中止，而且中止走的是下面那條路，不會畫選單。
  if (gRec.active() && !gSampling) return;

  // 通知畫面：按任意鍵提早關掉，不用等它自己消失。
  if (gNoticeUntil) { gNoticeUntil = 0; noticeDismissed(); return; }

  // 錯誤畫面：按任意鍵回到閒置。
  //
  // 不處理的話畫面會卡住 —— uiRender() 看到非 IDLE 就不畫選單，而選單狀態機
  // 還在前景，按上下只會在看不見的地方移動游標。這正是 Auto sampling 那個
  // 「按了沒反應」的同一類錯誤，加一個狀態就要記得加對應的出口。
  if (displayState() == TC_ST_ERROR) { showIdle(); return; }

  // 琴鍵模式：只有返回鍵有作用（離開），其餘讓 12 顆琴鍵自己去發聲。
  // 上/下鍵在這裡故意不做事 —— 演奏中被誤觸而突然移調會很難查。
  if (gKeys.enabled()) {
    if (k == UI_KEY_BACK) {
      gKeys.setEnabled(false);
      gUi.setSuspended(false);
      showIdle();
    }
    return;
  }

  // 這些模式會自己吃按鍵（採樣中按任意鍵結束、監看中按任意鍵停止），
  // 行為跟序列埠一致，不要讓選單插手。
  // setSuspended(false) 要在 showIdle() 之前：showIdle() 內部會重繪，
  // 順序反了的話那一次重繪還以為選單在讓位，於是停在閒置面板，
  // 要再按一下按鈕選單才會回來 —— 看起來又像「沒反應」。
  if (gSampling)  { endSampSession();
                    gUi.setSuspended(false); showIdle(); return; }
  if (gMonitor)   { gRec.endMonitor(); gMonitor = false;
                    Serial.println(F("[MON] 停止監看"));
                    gUi.setSuspended(false); showIdle(); return; }

  const char *cmd = gUi.feed(k);
  // 以 '?' 開頭的是選單內部用的，不是序列埠指令，攔下來自己處理
  if (cmd && strcmp(cmd, "?back") == 0) {     // 確認頁的 Cancel
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
    gUi.setSuspended(true);                 // 畫面讓給琴鍵面板
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
    // 兩種琴鍵共用同一個八度設定，不然切換輸入方式時音高會突然跳掉
    gKeys.setTranspose((int8_t)(gUiOctave * 12));
    gKbd.setTranspose((int8_t)(gUiOctave * 12));
    Serial.printf("[KEYS] 移調 %+d 個八度\n", (int)gUiOctave);
    return;
  }
  if (cmd && cmd[0]) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", cmd);
    Serial.printf("[UI] %s\n", buf);
    gMicGain = gUiMicGain;                    // 數值項的值先同步回去
    handleCommand(buf);

    // 剛剛跳出結果通知（清空成功之類）：不要把選單畫回去蓋掉它。
    //
    // 通知刻意用 TC_ST_IDLE 狀態，這樣時間到了自動 showIdle() 就能無縫接回選單。
    // 但也因此躲不過下面那個 displayState() 的判斷，要獨立擋一次。
    // 這裡不 suspend 選單 —— 通知消失後選單要自己回來，不需要使用者按返回。
    if (gNoticeUntil) return;

    // 指令有可能把畫面切給狀態面板（採樣、監看、錄音、演奏…）。
    // 這時讓選單退到背景，之後按返回叫回來 —— 不然選單狀態機還在前景，
    // 按上下會在看不見的情況下移動游標。
    if (displayState() != TC_ST_IDLE) { gUi.setSuspended(true); return; }
  }
  uiRender();
}

// 選單有兩個來源：麵板上的 4 顆實體按鈕，以及 USB 電腦鍵盤的方向鍵。
// 兩邊都走同一條 uiHandleKey()，所以行為必然一致 —— 不會出現「按鈕能進的
// 選項用鍵盤進不去」這種要在硬體上才發現的差異。
//
// 用迴圈把佇列一次清空：電腦鍵盤打字很快，一次 loop 只處理一顆的話，
// 連按會有明顯延遲。
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

  // MTP 要早一點起來：主機在列舉之後就開始問問題，等 SD／OLED 都初始化完
  // 才回應的話，Windows 那邊可能已經先把裝置判成沒有回應了。
  // 檔案系統是後面 SD 掛起來才註冊，begin() 與 addFilesystem() 分開沒關係。
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

  // 音訊記憶體：合成 2 路 + reverb 2 路 + 麥克風佇列 + 立體聲擷取佇列。
  // 擷取佇列在 SD 寫入時會暫時堆積，所以給得比之前寬鬆。
  AudioMemory(120);

  sgtl.enable();
  sgtl.volume(gVolume);

  applyInput();       // 裡面會處理 ADC 高通濾波器（見 applyInput 的說明）

  verb.roomsize(0.42f);
  verb.damping(0.55f);
  mixL.gain(0, 0.88f);  mixL.gain(1, 0.13f);
  mixR.gain(0, 0.88f);  mixR.gain(1, 0.13f);

  gModel.setBank(&gBank);
  synth.setModel(&gModel);
  synth.setMasterGain(0.18f);
  synth.setVibrato(50.0f, 4.8f);   // 上限與預設速率；實際深度/頻率都由 profile 量測決定

  gRec.begin(&recQueue);
  gCap.begin(&capQL, &capQR);
  gPlayer.begin(&synth);
  gKeys.begin(&synth);       // 12 顆實體琴鍵（接線說明見 keys.h）
  gKbd.begin(&synth);        // USB 電腦鍵盤當琴鍵（鍵位說明見 kbd_in.h）
  gKbd.usbBegin();           // USB Host 啟動（原本住在 midi_in，已移到 kbd_in）

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

  // 沒有音色時，所有發聲要求都會被安靜地丟掉。講清楚，別讓人以為是硬體壞了。
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
  // ---- USB 電腦鍵盤 ----
  // 放在 loop 最前面：USB Host 的列舉與收包都在這裡做，延遲直接決定手感。
  // 注意分析／訓練是阻塞的，那段期間鍵盤不會有反應（進去之前會先 allOff()）。
  gKbd.usbService();

  // ---- 實體琴鍵 / 按鈕 / OLED 選單 ----
  //
  // 必須放在 loop() 最前面。下面有好幾個 `return`（監看、採樣、錄音各一個），
  // 只要按鍵服務排在它們後面，那些模式底下按鈕就完全失效。
  //
  // 這正是「按下 Auto sampling 之後按鈕沒反應」的原因：畫面還在更新，看起來
  // 像當機，其實是按鍵根本沒被讀取，只有序列埠退得出來。
  // 放在最前面之後，以後再多加幾個 return 也不會重蹈覆轍。
  gKeys.service();
  uiTick();

  // ---- 結果通知的自動關閉 ----
  // 放在按鍵服務後面：如果剛才那一下按鍵已經把通知關掉，這裡就不會重複做。
  if (gNoticeUntil && (int32_t)(millis() - gNoticeUntil) >= 0) {
    gNoticeUntil = 0;
    noticeDismissed();
  }

  // ---- 序列埠 ----
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
    // 採樣模式中按任何鍵都是「結束」——手上拿著樂器，不該還要打完整指令
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

  // ---- 輸入電平監看 ----
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
      // 直流跟交流分開看。固定的直流偏移會讓上面那個 RMS 看起來「有訊號」，
      // 但它不是聲音 —— 而且會把環境自適應的觸發門檻整個抬高。
      Serial.printf("      直流 %+7.4f   交流 RMS %6.4f (%5.1f dB)%s\n",
                    dc, ac, 20.0f * log10f(ac + 1e-9f),
                    fabsf(dc) > 3.0f * (ac + 1e-6f) && fabsf(dc) > 0.005f
                      ? "   <- 直流蓋過交流，這不是收音問題" : "");

      // 三頻段。總電平正常但能量全在高頻，光看 RMS 完全看不出來 ——
      // 實測麥克風錄的鋼琴就是這樣：基頻幾乎不存在。
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
      // 用長條圖表示三個頻段，一眼看得出比例
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
      // 刻意不做自動判定。低頻佔比跟樂器種類高度相關 ——
      // 實測乾淨素材：鋼琴 50%、提琴 41%、長笛 27%、小號 17%。
      // 而錄壞的鋼琴是 18%，跟正常的小號一模一樣，任何固定門檻都會誤報。
      // 這裡只負責把數字顯示出來，判斷交給知道自己在彈什麼的人。
      displaySetLine(3, "target peak 0.3-0.8");
      displayForce();
    }
    return;
  }

  // ---- 連續採樣模式 ----
  if (gSampling) {
    if (gRec.service()) {                  // 剛錄完一個音
      onSampleCaptured();
      displaySetState(TC_ST_RECORDING, "SAMPLING - next note");
      char b[26];
      snprintf(b, sizeof(b), "captured %d", gSampCount);
      displaySetLine(0, b);
      // 手上拿著樂器的人不會在看序列埠，判定要出現在面板上。
      //
      // 錄音品質排在「換樂器」前面：錄壞了那一筆本來就不該進庫，
      // 而且使用者可以馬上改（調增益、重錄），值得優先看到。
      // 不合格的那一筆是「被丟掉」，不是「收下了但有點問題」——
       // 這兩件事對使用者的意義完全不同，面板上要分得出來。
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
          // 這 600 ms 不會觸發。不講的話，使用者會以為是壞了。
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
        // 門檻是自動的，所以一定要顯示它現在是多少、以及環境噪音多大 ——
        // 否則「怎麼都不觸發」跟「怎麼一直觸發」都會變成無從查起的抱怨。
        snprintf(b, sizeof(b), "amb%.3f thr%.3f", gRec.ambient(), gRec.threshold());
        displaySetLine(2, b);
        if (!gRec.calibrating() && gRec.ambient() >= gRec.baseThresh())
          displaySetLine(3, "room too noisy");
        else if (gRec.headroomDb() > 0.0f && gRec.headroomDb() < TC_TRIG_MIN_HEADROOM_DB)
          displaySetLine(3, "low margin - play up");
        else
          displaySetLine(3, "");
        displayService();

        // 沒接 OLED 的人也要看得到電平，否則「沒反應」時完全沒有線索
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
    return;                                // 採樣中不做其他事
  }

  // ---- 麥克風錄音（最優先，不能被 SD/OLED 卡住）----
  if (gRec.active()) {
    if (gRec.service()) {
      char buf[2] = {'a', 0};
      handleCommand(buf);                  // 錄完立刻分析
    } else {
      static uint32_t lastRecUi = 0;
      if (millis() - lastRecUi > 300) {    // 錄音中只做極低頻的畫面更新
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

  // ---- 演奏排程 + 同步擷取 ----
  //
  // 「演奏結束要把畫面還回去」以前寫在 gCap.active() 裡面，
  // 也就是只有「演奏並錄檔」那條路會恢復。按 r 錄音後自動演奏沒有開擷取，
  // 所以播完之後沒有任何東西把畫面救回來，永遠停在上一個狀態。
  // 現在改成獨立追蹤「剛剛還在播、現在不播了」，跟有沒有錄檔無關。
  {
    static bool wasPlaying = false;
    gPlayer.service();
    if (gCap.active()) {
      gCap.service();
      if (!gPlayer.playing()) {            // 演奏結束，收尾寫檔
        gCap.stop();
        gCapture = false;
        Serial.printf("[PLAY] 成品已存成 %s（內容 100%% 由加法合成產生）\n",
                      scoreGetMode() == TC_SCORE_CANON ? TC_CANON_PATH : TC_PLAY_PATH);
        // 這裡本來想送 MTP.send_DeviceResetEvent() 通知主機檔案變了，做不到：
        // MTP_Teensy 把所有 send_*Event() 包在 #if USE_EVENTS == 1 裡面，而那個
        // 巨集在函式庫自己的編譯單元裡是關的。名稱查找於是落到私有區那份同名
        // 宣告，編譯錯誤是「is private within this context」——不是「沒有這個成員」，
        // 所以第一眼會以為只是存取權限問題。
        //
        // 在 .ino 裡 #define USE_EVENTS 1 沒有用：Arduino 的函式庫是獨立編譯的，
        // 草稿碼的巨集傳不進去。要開就得改函式庫本身，那會讓這份專案綁在一份
        // 手改過的 MTP_Teensy 上，換台電腦就編不出來 —— 不值得。
        //
        // 實際影響很小：MTP 在演奏期間本來就沒有被服務（見 loop() 末端的條件），
        // 所以主機不可能快取到寫到一半的大小。檔案總管按 F5 重新整理就會看到
        // 正確的檔案。README 的 MTP 那一節有記這件事。
      }
    }
    const bool nowPlaying = gPlayer.playing();
    if (wasPlaying && !nowPlaying) {
      // 自然播完也要切回半音階。只在 stopPlay() 做是不夠的 ——
      // 卡農放到結束不會經過那裡，模式就會留在 CANON，
      // 下一次按 w 錄到的東西跟檔名對不起來。
      scoreSetMode(TC_SCORE_SCALE);
      showIdle();
    }
    wasPlaying = nowPlaying;
  }

  // 琴鍵模式的面板：顯示按著哪些音
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

  // ---- 畫面：現場彈奏 ----
  // 有人在彈的時候顯示鍵數與 CPU，放開一秒後回到閒置畫面。
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

  // ---- 畫面 ----
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

  // ---- 每 2 秒印一次資源用量 ----
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
  // 位置很重要，必須是 loop() 的最後一件事，而且要有下面這個條件。
  //
  // 監看／採樣／麥克風錄音那三段各自在上面 return 掉了，所以走到這裡就表示
  // 不在那些模式裡；分析與訓練是阻塞呼叫，那段期間 loop() 根本沒在跑。
  // 唯一會走到這裡卻同時在動 SD 的，就是「演奏並擷取」—— 那是 44.1 kHz
  // 立體聲即時寫檔，正是 MTP_Teensy issue #41 說會逾時失敗的情境，
  // 所以要明確擋掉。
  //
  // 代價是那幾段期間電腦上的磁碟機會沒有回應。這是刻意的取捨：
  // 寧可讓檔案總管轉圈圈，也不要在成品裡留下一個掉 block 的爆音。
#if TC_HAS_MTP
  if (gMtpOn && !gPlayer.playing() && !gCap.active()) MTP.loop();
#endif
}

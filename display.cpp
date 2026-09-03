#include "display.h"
#include "ui.h"

// ============================================================================
//  狀態（不管有沒有接面板都會維護，序列埠診斷也用得到）
// ============================================================================
static struct {
  TcState  state      = TC_ST_BOOT;
  char     detail[26] = {0};
  float    progress   = -1.0f;
  char     line[4][26] = {{0}};

  bool     sdOk       = false;
  bool     hasModel   = false;
  bool     hasProfile = false;

  int      trainSamples = 0;
  int      trainPitches = 0;

  float    f0         = 0.0f;
  char     noteName[8] = {0};

  bool     dirty      = true;

  // --- 選單 ---
  bool     menuOn     = false;
  char     menuTitle[26] = {0};
  char     menuRow[UI_VISIBLE_ROWS][26] = {{0}};
  int      menuN = 0, menuCur = 0, menuFirst = 0, menuTotal = 0;
  bool     menuEditing = false;
} gS;

static uint32_t gLastDraw = 0;

// ---------------------------------------------------------------------------
static void copyStr(char *dst, size_t cap, const char *src) {
  if (!src) { dst[0] = 0; return; }
  size_t i = 0;
  for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

void displaySetState(TcState s, const char *detail) {
  // 進入任何「忙碌」狀態時，狀態畫面自動把選單推開。
  // 這樣分析/訓練/演奏/錄音的進度條一定看得到，不用在每個流程裡各自記得
  // 去關選單 —— 那種散在各處的呼叫遲早會漏掉一個。
  if (s != TC_ST_IDLE && s != TC_ST_BOOT) gS.menuOn = false;
  gS.state = s;
  copyStr(gS.detail, sizeof(gS.detail), detail);
  gS.progress = -1.0f;
  for (int i = 0; i < 4; i++) gS.line[i][0] = 0;
  gS.dirty = true;
}
TcState displayState()      { return gS.state; }
bool    displayMenuVisible() { return gS.menuOn; }
void displaySetProgress(float p) { gS.progress = p; gS.dirty = true; }
void displaySetLine(int idx, const char *t) {
  if (idx < 0 || idx > 3) return;
  copyStr(gS.line[idx], sizeof(gS.line[idx]), t);
  gS.dirty = true;
}
void displaySetSystem(bool sdOk, bool hasModel, bool hasProfile) {
  gS.sdOk = sdOk; gS.hasModel = hasModel; gS.hasProfile = hasProfile; gS.dirty = true;
}
void displaySetTrainInfo(int samples, int pitches) {
  gS.trainSamples = samples; gS.trainPitches = pitches; gS.dirty = true;
}
void displaySetProfileInfo(float f0, const char *noteName) {
  gS.f0 = f0; copyStr(gS.noteName, sizeof(gS.noteName), noteName); gS.dirty = true;
}

// ============================================================================
// ---------------------------------------------------------------------------
//  選單狀態。刻意放在 #if TC_USE_OLED 之外 —— 它只寫 gS，一行 u8g2 都沒碰，
//  搬出來之後 tools/sim/display_test.cpp 就驗得到「選單不會蓋掉狀態面板」。
//  這條規則壞掉時螢幕還在更新，看起來不像當機，用眼睛盯 OLED 很難查。
// ---------------------------------------------------------------------------
void displaySetMenu(const char *title, const char (*rows)[26], int nRows,
                    int cursorRow, int firstRow, int totalRows, bool editing) {
  if (!title) { gS.menuOn = false; gS.dirty = true; return; }

  // 忙碌狀態下拒絕開選單。
  //
  // displaySetState() 進入忙碌狀態時已經把選單推開一次了，但那擋不住
  // 「之後才呼叫 displaySetMenu()」的人 —— Auto sampling 就是這樣壞的：
  // 指令把畫面切成採樣面板，選單卻在同一次按鍵處理的結尾又畫了回去，
  // 面板存在的時間短到看不見，電平表也永遠出不來。
  //
  // 擋在這裡而不是只改呼叫端，是因為呼叫端會越來越多，
  // 而「選單不該蓋掉狀態面板」這條規則只有一個地方講得清楚。
  if (gS.state != TC_ST_IDLE && gS.state != TC_ST_BOOT) {
    gS.menuOn = false;
    gS.dirty  = true;
    return;
  }

  gS.menuOn = true;
  copyStr(gS.menuTitle, sizeof(gS.menuTitle), title);
  gS.menuN = (nRows > UI_VISIBLE_ROWS) ? UI_VISIBLE_ROWS : nRows;
  for (int i = 0; i < gS.menuN; i++) copyStr(gS.menuRow[i], 26, rows[i]);
  gS.menuCur = cursorRow;
  gS.menuFirst = firstRow;
  gS.menuTotal = totalRows;
  gS.menuEditing = editing;
  gS.dirty = true;
}

#if TC_USE_OLED

#include <U8g2lib.h>
#include <Wire.h>

#if TC_OLED_SSD1306
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C  u8g2(U8G2_R0, U8X8_PIN_NONE);
  static const char *kDriverName = "SSD1306";
#elif TC_OLED_SH1106
  U8G2_SH1106_128X64_NONAME_F_HW_I2C   u8g2(U8G2_R0, U8X8_PIN_NONE);
  static const char *kDriverName = "SH1106";
#elif TC_OLED_SSD1309
  U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
  static const char *kDriverName = "SSD1309";
#else
  #error "config.h 裡要選一種 OLED 驅動 IC"
#endif

static bool gOledOk = false;

// 面板上用 ASCII。CJK 字型在 U8g2 裡動輒 1 MB 以上，而且 128x64 只放得下
// 兩三個中文字，資訊密度反而更差。序列埠仍然是完整中文。
#define FONT_SMALL  u8g2_font_5x8_tf         // 25 字 x 8 列
#define FONT_TITLE  u8g2_font_6x12_tf

// ---------------------------------------------------------------------------
void displayScanI2C() {
  Wire.begin();
  Serial.println(F("[I2C] 掃描匯流排..."));
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("      0x%02X", a);
      if (a == 0x0A) Serial.print(F("  <- SGTL5000 (Audio Shield)"));
      if (a == 0x3C || a == 0x3D) Serial.print(F("  <- OLED"));
      Serial.println();
      found++;
    }
  }
  if (found == 0)
    Serial.println(F("      沒有任何裝置。檢查 SDA=18 / SCL=19 接線與電源。"));
}

void displayBegin() {
  displayScanI2C();
  gOledOk = u8g2.begin();
  if (!gOledOk) {
    Serial.printf("[OLED] %s 初始化失敗。若面板其實是別顆 IC，"
                  "改 config.h 的 TC_OLED_* 三個開關。\n", kDriverName);
    return;
  }
  u8g2.setBusClock(TC_OLED_I2C_HZ);
  u8g2.setFontPosTop();
  Serial.printf("[OLED] %s 128x64 就緒\n", kDriverName);

  u8g2.clearBuffer();
  u8g2.setFont(FONT_TITLE);
  u8g2.drawStr(14, 18, "TimbreClone");
  u8g2.setFont(FONT_SMALL);
  u8g2.drawStr(10, 36, "Teensy 4.1 timbre clone");
  u8g2.drawStr(30, 48, "booting...");
  u8g2.sendBuffer();
}

// ---------------------------------------------------------------------------
static void drawBar(int x, int y, int w, int h, float frac) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  u8g2.drawFrame(x, y, w, h);
  int inner = (int)((w - 4) * frac);
  if (inner > 0) u8g2.drawBox(x + 2, y + 2, inner, h - 4);
}

static const char *stateName(TcState s) {
  switch (s) {
    case TC_ST_RECORDING: return "RECORDING";
    case TC_ST_ANALYZING: return "ANALYZING";
    case TC_ST_TRAINING:  return "TRAINING";
    case TC_ST_PLAYING:   return "PLAYING";
    case TC_ST_ERROR:     return "ERROR";
    case TC_ST_IDLE:      return "READY";
    default:              return "BOOT";
  }
}

// ---------------------------------------------------------------------------
//  選單畫面
//
//  128x64 用 6x10 的字型大約 21 字 x 6 列。扣掉標題列與底部提示，
//  中間剛好放 4 列 —— 這就是 UI_VISIBLE_ROWS = 4 的由來。
static void drawMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);

  u8g2.drawStr(0, 0, gS.menuTitle);
  // 右上角顯示「第幾項 / 共幾項」，長清單捲動時才知道自己在哪
  if (gS.menuTotal > UI_VISIBLE_ROWS) {
    // 24 bytes：兩個 int 最長各 11 字元加一個斜線加結尾。
    // 實際上選單頂多十幾項，但編譯器不知道，開夠大比加 -Wno- 誠實。
    char pos[24];
    snprintf(pos, sizeof(pos), "%d/%d", (int)gS.menuCur + 1, (int)gS.menuTotal);
    u8g2.drawStr(128 - u8g2.getStrWidth(pos), 0, pos);
  }
  u8g2.drawHLine(0, 10, 128);

  for (int i = 0; i < gS.menuN && i < UI_VISIBLE_ROWS; i++) {
    const int y = 14 + i * 10;
    const bool sel = (gS.menuFirst + i) == gS.menuCur;
    if (sel) {
      // 反白整列比畫一個小箭頭好認 —— 在 OLED 上小箭頭很容易看漏
      u8g2.drawBox(0, y - 1, 128, 10);
      u8g2.setDrawColor(0);
      u8g2.drawStr(3, y, gS.menuRow[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(3, y, gS.menuRow[i]);
    }
  }

  u8g2.drawHLine(0, 55, 128);
  u8g2.drawStr(0, 56, gS.menuEditing ? "UP/DN value   OK done"
                                     : "UP/DN move  OK enter");
  u8g2.sendBuffer();
}

static void draw() {
  if (gS.menuOn) { drawMenu(); return; }
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);

  // ---- 標題列 -----------------------------------------------------------
  u8g2.drawStr(0, 0, stateName(gS.state));
  {
    // 右上角：音源模式，這是「卡農到底用什麼發聲」的即時證據
    const char *mode = gS.hasModel ? "MLP" : "KEYFR";
    int w = u8g2.getStrWidth(mode);
    u8g2.drawStr(128 - w, 0, mode);
  }
  u8g2.drawHLine(0, 10, 128);

  // ---- 主要區塊 ---------------------------------------------------------
  int y = 14;
  if (gS.detail[0]) { u8g2.drawStr(0, y, gS.detail); y += 9; }

  if (gS.progress >= 0.0f) {
    drawBar(0, y, 104, 9, gS.progress);
    char pct[8];
    snprintf(pct, sizeof(pct), "%3d%%", (int)(gS.progress * 100.0f + 0.5f));
    u8g2.drawStr(106, y + 1, pct);
    y += 12;
  }

  for (int i = 0; i < 4 && y <= 54; i++) {
    if (gS.line[i][0]) { u8g2.drawStr(0, y, gS.line[i]); y += 9; }
  }

  // ---- 底部狀態列 -------------------------------------------------------
  u8g2.drawHLine(0, 55, 128);
  char foot[32];
  if (gS.state == TC_ST_IDLE || gS.state == TC_ST_BOOT) {
    if (gS.hasProfile && gS.f0 > 0.0f)
      snprintf(foot, sizeof(foot), "%s %.0fHz  trn %d/%dp",
               gS.noteName[0] ? gS.noteName : "--", gS.f0,
               gS.trainSamples, gS.trainPitches);
    else
      snprintf(foot, sizeof(foot), "no timbre  SD:%s", gS.sdOk ? "ok" : "--");
  } else {
    snprintf(foot, sizeof(foot), "SD:%s  trn %d/%dp",
             gS.sdOk ? "ok" : "--", gS.trainSamples, gS.trainPitches);
  }
  u8g2.drawStr(0, 56, foot);

  u8g2.sendBuffer();
}

void displayService() {
  if (!gOledOk) return;
  uint32_t now = millis();
  if (!gS.dirty && (now - gLastDraw) < 1000) return;      // 沒變就 1 秒心跳一次
  if ((now - gLastDraw) < TC_OLED_REFRESH_MS) return;
  gLastDraw = now;
  gS.dirty  = false;
  draw();
}

void displayForce() {
  if (!gOledOk) return;
  gLastDraw = millis();
  gS.dirty  = false;
  draw();
}

// ============================================================================
#else   // TC_USE_OLED == 0：全部變空函式

void displayBegin()   {}
void displayScanI2C() {}
void displayService() {}
void displayForce()   {}

#endif

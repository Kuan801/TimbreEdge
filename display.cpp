#include "display.h"
#include "ui.h"

// ============================================================================
//  State (maintained with or without a panel; serial diagnostics use it too)
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

  // --- Menu ---
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
  // Entering any "busy" state makes the status screen push the menu aside, so
  // the analyse/train/play/record progress bar is always visible and no flow
  // has to remember to close the menu itself -- calls scattered around like
  // that always end up missing one.
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
//  Menu state. Deliberately outside #if TC_USE_OLED -- it only writes gS and
//  never touches a line of u8g2, so once it lives out here
//  tools/sim/display_test.cpp can verify "the menu must not cover the status
//  panel". When that rule breaks the screen keeps updating, so it looks nothing
//  like a hang and eyeballing the OLED will hardly ever find it.
// ---------------------------------------------------------------------------
void displaySetMenu(const char *title, const char (*rows)[26], int nRows,
                    int cursorRow, int firstRow, int totalRows, bool editing) {
  if (!title) { gS.menuOn = false; gS.dirty = true; return; }

  // Refuse to open the menu while busy.
  //
  // displaySetState() already pushes the menu aside when it enters a busy
  // state, but that does not stop whoever calls displaySetMenu() afterwards --
  // that is exactly how Auto sampling broke: the command switched the screen to
  // the sampling panel, then the menu was drawn back over it at the end of the
  // same key handler, so the panel existed too briefly to see and the level
  // meter never appeared.
  //
  // Blocked here rather than only at the call sites, because the call sites
  // keep multiplying, and "the menu must not cover the status panel" is a rule
  // that can only be stated clearly in one place.
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

// ASCII on the panel. A CJK font in U8g2 easily runs past 1 MB, and 128x64 only
// fits two or three glyphs, so density is worse. Serial is still full Chinese.
#define FONT_SMALL  u8g2_font_5x8_tf         // 25 chars x 8 rows
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
//  Menu screen
//
//  128x64 with the 6x10 font is about 21 chars x 6 rows. Minus the title row
//  and the bottom hint, exactly 4 rows are left -- hence UI_VISIBLE_ROWS = 4.
static void drawMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);

  u8g2.drawStr(0, 0, gS.menuTitle);
  // Top right shows "item / total", so a long scrolling list still says where you are
  if (gS.menuTotal > UI_VISIBLE_ROWS) {
    // 24 bytes: two ints of at most 11 chars each, a slash, and the terminator.
    // Menus hold a dozen items at most, but the compiler cannot know that, and
    // sizing it big enough is more honest than adding a -Wno- flag.
    char pos[24];
    snprintf(pos, sizeof(pos), "%d/%d", (int)gS.menuCur + 1, (int)gS.menuTotal);
    u8g2.drawStr(128 - u8g2.getStrWidth(pos), 0, pos);
  }
  u8g2.drawHLine(0, 10, 128);

  for (int i = 0; i < gS.menuN && i < UI_VISIBLE_ROWS; i++) {
    const int y = 14 + i * 10;
    const bool sel = (gS.menuFirst + i) == gS.menuCur;
    if (sel) {
      // Inverting the whole row reads better than a small arrow -- arrows are easy to miss on an OLED
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

  // ---- Title row ----------------------------------------------------------
  u8g2.drawStr(0, 0, stateName(gS.state));
  {
    // Top right: the voice mode -- live evidence of what the canon is actually sounding with
    const char *mode = gS.hasModel ? "MLP" : "KEYFR";
    int w = u8g2.getStrWidth(mode);
    u8g2.drawStr(128 - w, 0, mode);
  }
  u8g2.drawHLine(0, 10, 128);

  // ---- Main area ----------------------------------------------------------
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

  // ---- Bottom status bar --------------------------------------------------
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
  if (!gS.dirty && (now - gLastDraw) < 1000) return;      // nothing changed: one heartbeat per second
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
#else   // TC_USE_OLED == 0: everything becomes an empty stub

void displayBegin()   {}
void displayScanI2C() {}
void displayService() {}
void displayForce()   {}

#endif

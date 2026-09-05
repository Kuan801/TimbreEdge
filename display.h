// ============================================================================
//  display.h  -  128x64 OLED status panel
//
//  Driven through U8g2, because one library covers SSD1306 / SH1106 / SSD1309:
//  swapping the module is a one-line change in config.h, not a change of library.
//
//  The interface is deliberately "set state + rate-limited refresh from loop()":
//    displaySet*()     only updates state in memory; it is cheap, call it freely
//    displayService()  called from loop(); actually redraws at most every
//                      TC_OLED_REFRESH_MS
//    displayForce()    redraw right now (used inside blocking flows such as
//                      analysis and training)
//
//  Why rate-limit: a full 128x64 frame is 1024 bytes, about 20 ms over 400 kHz
//  I2C. Refreshing every loop would stall loop() completely. Player schedules on
//  accumulated time so it will not drift, but long blocking still has to be
//  avoided while recording.
//
//  With TC_USE_OLED = 0 everything here becomes an empty function and the rest of
//  the code is unchanged.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"

enum TcState : uint8_t {
  TC_ST_BOOT = 0,
  TC_ST_IDLE,
  TC_ST_RECORDING,
  TC_ST_ANALYZING,
  TC_ST_TRAINING,
  TC_ST_PLAYING,
  TC_ST_ERROR
};

void displayBegin();
void displayScanI2C();                 // Boot diagnostic: print which addresses are on the bus

void displaySetState(TcState s, const char *detail = nullptr);

// Which state we are in. Lets the menu tell whether a status panel currently owns
// the display -- while it does, the menu must not draw over it, or the panel is
// wiped out the instant it appears.
TcState displayState();

// Whether the menu or a status panel is on screen right now.
// Exposed as a public query so that the rule "the menu must not cover a status
// panel" can be tested on a desktop -- when it breaks, the symptom is "pressing
// a key does nothing", which is very hard to spot by watching the OLED.
bool displayMenuVisible();
void displaySetProgress(float p);      // 0..1; negative = do not draw the progress bar
void displaySetLine(int idx, const char *text);   // Free-text lines, idx 0..3

// Status-line information (top right and bottom)
void displaySetSystem(bool sdOk, bool hasModel, bool hasProfile);
void displaySetTrainInfo(int samples, int pitches);
void displaySetProfileInfo(float f0, const char *noteName);

// Menu screen. A nullptr title means "not in the menu" and returns to the
// previous status screen. The strings to draw are handed in rather than having
// display.cpp read Ui -- that way ui.cpp need not know u8g2 exists, which is
// what makes it testable on a desktop.
void displaySetMenu(const char *title, const char (*rows)[26], int nRows,
                    int cursorRow, int firstRow, int totalRows, bool editing);

void displayService();                 // Call from loop()
void displayForce();                   // Redraw immediately

// ============================================================================
//  display.h  -  128x64 OLED 狀態面板
//
//  用 U8g2 驅動，因為它一個函式庫就涵蓋 SSD1306 / SH1106 / SSD1309，
//  換模組只要改 config.h 一行，不必換函式庫。
//
//  介面刻意做成「設定狀態 + 由 loop() 限流刷新」：
//    displaySet*()  只更新記憶體裡的狀態，極快，可以隨便呼叫
//    displayService()  在 loop() 呼叫，內部每 TC_OLED_REFRESH_MS 才真的畫一次
//    displayForce()    立刻畫（分析/訓練這種阻塞流程中用）
//
//  為什麼要限流：128x64 全畫面 = 1024 bytes，@400kHz I2C 約 20 ms。
//  每次 loop 都刷會把 loop() 卡死。Player 用累積時間排程所以不會漂移，
//  但錄音時仍要避免長時間阻塞。
//
//  TC_USE_OLED = 0 時全部變成空函式，程式其他部分不用改。
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
void displayScanI2C();                 // 開機診斷：印出匯流排上有哪些位址

void displaySetState(TcState s, const char *detail = nullptr);

// 目前是哪個狀態。給選單判斷「畫面現在有沒有被狀態面板佔著」——
// 佔著的時候選單不能畫回去，否則面板一瞬間就被蓋掉。
TcState displayState();

// 現在畫面上是選單還是狀態面板。
// 拉成公開查詢是為了讓「選單不該蓋掉狀態面板」這條規則在桌機上驗得到 ——
// 它壞掉的表現是「按了沒反應」，用眼睛看 OLED 很難查出來。
bool displayMenuVisible();
void displaySetProgress(float p);      // 0..1；負值 = 不畫進度條
void displaySetLine(int idx, const char *text);   // idx 0..3 的自由文字列

// 狀態列資訊（右上角與底部）
void displaySetSystem(bool sdOk, bool hasModel, bool hasProfile);
void displaySetTrainInfo(int samples, int pitches);
void displaySetProfileInfo(float f0, const char *noteName);

// 選單畫面。傳 nullptr 的 title 表示「不在選單」，回到原本的狀態畫面。
// 用「把要畫的字串交進來」而不是讓 display.cpp 去讀 Ui —— 這樣 ui.cpp
// 不必知道 u8g2 的存在，桌機上才測得到。
void displaySetMenu(const char *title, const char (*rows)[26], int nRows,
                    int cursorRow, int firstRow, int totalRows, bool editing);

void displayService();                 // loop() 裡呼叫
void displayForce();                   // 立刻重繪

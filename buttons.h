// ============================================================================
//  buttons.h  -  4 顆輕觸開關的去彈跳與邊緣偵測
//
//  跟 ui.h 分開的理由：這裡是唯一碰硬體的部分（digitalRead / millis），
//  ui.cpp 是純狀態機。分開之後選單邏輯在桌機測得到。
//
//  接法：按鈕一腳接 Teensy 腳位，另一腳接 GND。用 INPUT_PULLUP，
//  所以不用外接電阻，按下去讀到 LOW。
//
//  去彈跳用「狀態穩定 TC_BTN_DEBOUNCE_MS 才承認」的作法，而不是按下就
//  delay()。輕觸開關的彈跳通常 1~5 ms，接觸不良的會更久；用 delay 會把
//  audio 的 loop 卡住，而這支程式的 loop 還要餵 USB Host 和 SD 寫入。
//
//  上下鍵有自動連發（按住 400 ms 後每 120 ms 一次），調 micGain 這種
//  0~63 的值時才不用按 63 下。
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "ui.h"

class Buttons {
public:
  void begin();

  // 在 loop() 裡呼叫。回傳這一輪產生的按鍵事件（一次一個），沒有就 UI_KEY_NONE。
  UiKey poll();

  // 有沒有任何一顆正被按著（開機自檢用得到）
  bool anyDown() const;

private:
  struct Btn {
    uint8_t  pin;
    UiKey    key;
    bool     stable   = false;   // 去彈跳之後的狀態，true = 按下
    bool     raw      = false;
    uint32_t changed  = 0;       // raw 最後一次改變的時間
    uint32_t downAt   = 0;       // stable 變成按下的時間
    uint32_t lastRep  = 0;       // 最後一次連發的時間
    bool     repeats  = false;   // 這顆要不要連發
  };
  Btn _b[4];
};

extern Buttons gButtons;

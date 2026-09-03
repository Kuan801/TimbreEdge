// ============================================================================
//  kbd_in.h  -  用一般 USB 電腦鍵盤當琴鍵，順便當選單的方向鍵
//
//  插在 USB Host 埠上就能用，一條線都不用接。
//
//  --- 鍵位對應（DAW 的打字鍵盤慣例）------------------------------------------
//
//      上排 = 高八度            2 3   5 6 7          <- 黑鍵
//                              Q W E R T Y U        <- 白鍵   C5 ~ B5
//
//      下排 = 音色庫原位         S D   G H J          <- 黑鍵
//                              Z X C V B N M        <- 白鍵   C4 ~ B4
//
//      左/右方向鍵   整體移調 -1 / +1 個八度
//      空白鍵        全部停音
//      Esc           離開琴鍵模式
//
//  在選單裡（沒進琴鍵模式時）：
//      上/下方向鍵 = 移動游標   Enter = 確定   Esc / Backspace = 返回
//
//  --- 兩個必須先知道的限制 ---------------------------------------------------
//
//  1) 和弦數有上限。USB HID 的 boot protocol 一次只回報 6 個按鍵，而便宜的
//     薄膜鍵盤更慘 —— 沒有防鬼鍵設計的話，某些三鍵組合就會漏鍵或多出沒按的鍵。
//     這是鍵盤硬體的限制，程式端無解。想彈四音以上的和弦要挑標示
//     「N-key rollover」或「anti-ghosting」的機械鍵盤。
//
//  2) 沒有力度。HID 鍵盤只回報按下/放開，測不出力道。跟實體按鈕一樣。
//
//  --- 為什麼用 raw keycode 而不是 ASCII -------------------------------------
//
//  attachRawPress() 給的是 HID usage code，對應的是「鍵盤上的實體位置」。
//  用 ASCII 的話，換成非 QWERTY 佈局（或使用者切到中文輸入法）就全亂了。
//  實體位置在任何佈局下都一樣，Z 鍵永遠是左下角那一顆。
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "ui.h"

// 只用到指標，前置宣告就夠。
// 這麼寫的用意是讓 tools/sim/kbd_test.cpp 能塞一個假的合成器進來 ——
// 有了它，「Z 鍵到底送出哪個音高」在桌機上就驗得到，不必燒進去用耳朵聽。
class AudioSynthAdditive;

// HID usage code（跟鍵盤語言無關，是實體位置）
#define HID_A 0x04
#define HID_B 0x05
#define HID_C 0x06
#define HID_D 0x07
#define HID_E 0x08
#define HID_F 0x09
#define HID_G 0x0A
#define HID_H 0x0B
#define HID_J 0x0D
#define HID_M 0x10
#define HID_N 0x11
#define HID_Q 0x14
#define HID_R 0x15
#define HID_S 0x16
#define HID_T 0x17
#define HID_U 0x18
#define HID_V 0x19
#define HID_W 0x1A
#define HID_X 0x1B
#define HID_Y 0x1C
#define HID_Z 0x1D
#define HID_2 0x1F
#define HID_3 0x20
#define HID_5 0x22
#define HID_6 0x23
#define HID_7 0x24
#define HID_ENTER     0x28
#define HID_ESC       0x29
#define HID_BACKSPACE 0x2A
#define HID_SPACE     0x2C
#define HID_RIGHT     0x4F
#define HID_LEFT      0x50
#define HID_DOWN      0x51
#define HID_UP        0x52

#define TC_KBD_MAX_DOWN 8      // 同時追蹤幾顆（HID boot protocol 上限是 6）

class KbdInput {
public:
  void begin(AudioSynthAdditive *synth);

  // 餵一個 HID 鍵碼進來。回傳「這一顆有沒有被當成選單按鍵」——
  // 有的話呼叫端要把對應的 UiKey 丟給選單。
  //
  // 拉成公開介面的理由跟 MidiInput::feed() 一樣：這段邏輯跟 USB 無關，
  // 桌機測得到。之前 USB MIDI 只能在硬體上驗，來回花掉好幾輪。
  UiKey feed(uint8_t hidCode, bool pressed);

  // USB 回呼專用：呼叫 feed()，並把選單按鍵排進佇列。
  //
  // 為什麼要排隊而不是直接餵給 gUi：回呼是在 usbHost.Task() 裡面被呼叫的，
  // 而 Ui::feed() 會回傳指令字串、由 .ino 拿去執行 —— 那些指令（分析、訓練）
  // 動輒阻塞好幾秒。在 USB 的處理迴圈裡做那種事會把後續封包丟掉。
  // 排進佇列、等回到 loop() 再處理，就沒這個問題。
  void feedFromUsb(uint8_t hidCode, bool pressed);
  UiKey popUiKey();                      // 沒有待處理的就回 UI_KEY_NONE

  void setEnabled(bool on);              // 進出琴鍵模式
  bool enabled() const { return _on; }

  uint32_t pressCount() const { return _presses; }
  uint32_t noteCount()  const { return _notes; }

  // USB Host 的啟動與輪詢。實作在 kbd_in.cpp 的 TC_USE_USB_KBD 區段裡，
  // 桌機模擬編成空函式。
  void usbBegin();
  void usbService();

  bool connected() const { return _connected; }

  void setTranspose(int8_t semi);
  int8_t transpose() const { return _transpose; }

  void allOff();

  int  downCount() const;
  void downText(char *out, size_t cap) const;

private:
  // 這個鍵碼對應到哪個音（相對 C4 的半音數）；不是琴鍵就回 -1
  static int noteOf(uint8_t hidCode);

  AudioSynthAdditive *_synth = nullptr;
  bool    _on = false;
  bool    _connected = false;
  int8_t  _transpose = 0;

  struct Down { uint8_t code; uint8_t midi; };
  Down _down[TC_KBD_MAX_DOWN];
  int  _nDown = 0;

  uint32_t _presses = 0;                 // 收到的按下事件總數（診斷用）
  uint32_t _notes   = 0;                 // 其中真的變成音符的

  // 佇列不必 volatile：USBHost_t36 的回呼是從 usbHost.Task() 裡分派的，
  // 而 Task() 跟 uiTick() 都在 loop() 同一條執行緒上，兩邊不會同時動它。
  static const int QN = 8;
  UiKey   _q[QN];
  uint8_t _qHead = 0, _qTail = 0;
};

extern KbdInput gKbd;

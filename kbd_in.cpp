#include "kbd_in.h"
#include <Arduino.h>   // Serial / F()：真機是 USBHost_t36 順便帶進來的，
                       // 但別依賴那個間接路徑，桌機 usbcheck 就會漏掉
#include "config.h"    // TC_USE_USB_KBD
#include <stdio.h>

// 這一行 #error 是為了一個真的發生過、而且完全無聲的 bug。
//
// config.h 從 midi_in.cpp 搬過來的時候沒有跟著搬，結果 TC_USE_USB_KBD 沒有定義，
// 而 `#if 未定義的巨集` 在 C++ 裡的值是 0 —— 底下整段 USB Host 就這樣被
// 靜靜地編掉了，換成兩個空函式。編得過、燒得進去、開機也正常，
// 只是插上鍵盤永遠沒反應。
//
// 更糟的是 usbcheck 抓不到：它自己會 -DTC_USE_USB_KBD=1，所以在那個目標底下
// 這段程式碼永遠是活的。測試把它要驗的那個條件自己補上了。
//
// 有了這行，「忘了 include config.h」會變成編譯錯誤而不是沉默的行為改變。
#if !defined(TC_USE_USB_KBD)
  #error "TC_USE_USB_KBD 沒有定義 —— config.h 沒被 include 進來。"
#endif

// ============================================================================
//  USB Host
//
//  這一段以前住在 midi_in.cpp。USB MIDI 鍵盤從頭到尾沒收到過任何封包
//  （裝置列舉正常、描述元正確、bulk IN 端點也在，就是沒有資料），
//  查了很久沒有結論，最後決定移除，改用一般 USB 電腦鍵盤當輸入。
//  舊的程式碼放在 舊版備份/midi_in.cpp，要回頭查隨時找得到。
//
//  USBHost_t36 的驅動物件是在建構子裡把自己掛進一條靜態串列的，
//  跨檔案的靜態初始化順序在 C++ 未定義，所以全部集中在同一個 .cpp 宣告，
//  順序才由書寫順序決定。
//
//  KeyboardController 繼承 USBHIDInput 而不是 USBDriver —— 它自己不認領介面，
//  要靠 USBHIDParser 認領 HID 介面再把報告轉給它。少了 USBHIDParser 會編得過、
//  裝置也列舉得到，但一個按鍵事件都收不到。三個是為了複合裝置
//  （鍵盤 + 多媒體鍵 + 滑鼠各佔一個介面）。
// ============================================================================
#if TC_USE_USB_KBD

#include <USBHost_t36.h>

static USBHost           usbHost;
static USBHub            usbHub1(usbHost);
static USBHub            usbHub2(usbHost);
static USBHIDParser      usbHid1(usbHost);
static USBHIDParser      usbHid2(usbHost);
static USBHIDParser      usbHid3(usbHost);
static KeyboardController usbKbd(usbHost);

// 回呼是 C 函式指標，要用檔案範圍的自由函式轉手。
//
// 用 attachRawPress 而不是 attachPress：raw 給的是 HID usage code，對應
// 「鍵盤上的實體位置」；attachPress 給的是已經套過佈局的 unicode，換一把
// AZERTY 鍵盤或切到別的輸入法就全跑掉了。琴鍵要的是位置。
static void hKeyRawPress(uint8_t code)   { gKbd.feedFromUsb(code, true);  }
static void hKeyRawRelease(uint8_t code) { gKbd.feedFromUsb(code, false); }

void KbdInput::usbBegin() {
  usbHost.begin();
  usbKbd.attachRawPress(hKeyRawPress);
  usbKbd.attachRawRelease(hKeyRawRelease);
  Serial.println(F("[KBD] USB Host 已啟動，等待電腦鍵盤插入…"));
}

void KbdInput::usbService() {
  usbHost.Task();

  const bool kb = (bool)usbKbd;
  if (kb != _connected) {
    _connected = kb;
    if (kb) {
      const char *p = (const char *)usbKbd.product();
      Serial.printf("[KBD] 電腦鍵盤已連接：%.31s\n", p ? p : "USB Keyboard");
      Serial.println(F("      Z S X D C V G B H N J M = C4~B4，"
                       "Q 2 W 3 E R 5 T 6 Y 7 U = C5~B5"));
      Serial.println(F("      左右方向鍵換八度，空白鍵全部停音；"
                       "上下方向鍵與 Enter/Esc 操作選單"));
    } else {
      Serial.println(F("[KBD] 電腦鍵盤已拔除"));
      allOff();
    }
  }
}

#else   // ------------------------------------------------------ 桌機模擬 --

void KbdInput::usbBegin()   {}
void KbdInput::usbService() {}

#endif

// 桌機測試會自己提供一個假的 AudioSynthAdditive（見 tools/sim/kbd_test.cpp），
// 那時候不能再把真的定義拉進來。正式編譯時這個巨集沒定義，行為完全照舊。
#ifndef TC_KBD_FAKE_SYNTH
#include "additive_synth.h"
#endif

KbdInput gKbd;

// ---------------------------------------------------------------------------
//  鍵碼 -> 相對 C4 的半音數
//
//  用查表而不是一長串 switch，是因為之後想改鍵位（例如把下排改成從 C3 起）
//  只要動這張表，不用碰邏輯。表很短，線性掃過去比雜湊還快。
// ---------------------------------------------------------------------------
struct KeyMap { uint8_t code; int8_t semi; };

static const KeyMap kMap[] = {
  // 下排：C4 ~ B4（跟音色庫的原位一致，也跟 12 顆實體按鍵一致）
  { HID_Z,  0 }, { HID_S,  1 }, { HID_X,  2 }, { HID_D,  3 },
  { HID_C,  4 }, { HID_V,  5 }, { HID_G,  6 }, { HID_B,  7 },
  { HID_H,  8 }, { HID_N,  9 }, { HID_J, 10 }, { HID_M, 11 },
  // 上排：C5 ~ B5
  { HID_Q, 12 }, { HID_2, 13 }, { HID_W, 14 }, { HID_3, 15 },
  { HID_E, 16 }, { HID_R, 17 }, { HID_5, 18 }, { HID_T, 19 },
  { HID_6, 20 }, { HID_Y, 21 }, { HID_7, 22 }, { HID_U, 23 },
};
static const int kMapN = (int)(sizeof(kMap) / sizeof(kMap[0]));

static const char *kNoteName[12] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

int KbdInput::noteOf(uint8_t hidCode) {
  for (int i = 0; i < kMapN; i++)
    if (kMap[i].code == hidCode) return kMap[i].semi;
  return -1;
}

void KbdInput::begin(AudioSynthAdditive *synth) {
  _synth = synth;
  _on    = true;         // 跟 MIDI 一樣：插上去就能彈，不用先進什麼模式
  _nDown = 0;
}

void KbdInput::setEnabled(bool on) {
  if (on == _on) return;
  _on = on;
  if (!on) allOff();
}

void KbdInput::allOff() {
  if (_synth)
    for (int i = 0; i < _nDown; i++) _synth->noteOff((float)_down[i].midi);
  _nDown = 0;
}

void KbdInput::setTranspose(int8_t semi) {
  if (semi < -36) semi = -36;
  if (semi >  36) semi =  36;
  if (semi == _transpose) return;
  // 先把還響著的音收掉。不收的話，放開時會用「新的」音高去 noteOff，
  // 關不掉舊的那顆 —— 就變成移調一次卡一個音，越彈越吵。
  allOff();
  _transpose = semi;
}

int KbdInput::downCount() const { return _nDown; }

void KbdInput::downText(char *out, size_t cap) const {
  out[0] = 0;
  size_t used = 0;
  for (int i = 0; i < _nDown && used + 5 < cap; i++) {
    const int pc = _down[i].midi % 12;
    int n = snprintf(out + used, cap - used, "%s%s", used ? " " : "", kNoteName[pc]);
    if (n > 0) used += (size_t)n;
  }
  if (!used) snprintf(out, cap, "---");
}

// ---------------------------------------------------------------------------
//  主要入口
//
//  回傳值是「這一顆要不要當成選單按鍵」。音符鍵跟方向鍵不會互相干擾
//  （字母 vs 方向鍵是不同鍵碼），所以不需要分模式 —— 一邊瀏覽選單一邊
//  試音是可以的，這反而比「先進琴鍵模式才能彈」順手。
// ---------------------------------------------------------------------------
UiKey KbdInput::feed(uint8_t hidCode, bool pressed) {
  // ---- 選單鍵 ------------------------------------------------------------
  switch (hidCode) {
    case HID_UP:        return pressed ? UI_KEY_UP   : UI_KEY_NONE;
    case HID_DOWN:      return pressed ? UI_KEY_DOWN : UI_KEY_NONE;
    case HID_ENTER:     return pressed ? UI_KEY_OK   : UI_KEY_NONE;
    case HID_ESC:
    case HID_BACKSPACE: return pressed ? UI_KEY_BACK : UI_KEY_NONE;

    case HID_LEFT:
      if (pressed) setTranspose((int8_t)(_transpose - 12));
      return UI_KEY_NONE;
    case HID_RIGHT:
      if (pressed) setTranspose((int8_t)(_transpose + 12));
      return UI_KEY_NONE;
    case HID_SPACE:
      if (pressed) allOff();
      return UI_KEY_NONE;
    default:
      break;
  }

  // ---- 音符鍵 ------------------------------------------------------------
  const int semi = noteOf(hidCode);
  if (semi < 0 || !_on || !_synth) return UI_KEY_NONE;

  if (pressed) {
    // 自動重複：作業系統會重送，USB HID 不會 —— 但鍵盤韌體偶爾會重送同一份
    // 報告，所以還是擋一下，免得同一個音被 noteOn 兩次、放開只關掉一次。
    for (int i = 0; i < _nDown; i++) if (_down[i].code == hidCode) return UI_KEY_NONE;
    if (_nDown >= TC_KBD_MAX_DOWN) return UI_KEY_NONE;

    int midi = 60 + semi + _transpose;         // 60 = C4
    if (midi < 0)   midi = 0;
    if (midi > 127) midi = 127;

    _down[_nDown].code = hidCode;
    _down[_nDown].midi = (uint8_t)midi;
    _nDown++;
    _notes++;
    // 力度固定。HID 鍵盤只回報按下/放開，量不出力道 —— 跟實體按鈕一樣的限制。
    _synth->noteOn((float)midi, 0.85f, 0.5f);

  } else {
    for (int i = 0; i < _nDown; i++) {
      if (_down[i].code != hidCode) continue;
      // 用當初送出去的那個音高關掉，不是重新算 —— 中途移調過就會對不上
      _synth->noteOff((float)_down[i].midi);
      _down[i] = _down[--_nDown];
      break;
    }
  }
  return UI_KEY_NONE;
}

// ---------------------------------------------------------------------------
//  USB 回呼與 loop() 之間的交接
// ---------------------------------------------------------------------------
void KbdInput::feedFromUsb(uint8_t hidCode, bool pressed) {
  if (pressed) _presses++;
  const UiKey k = feed(hidCode, pressed);
  if (k == UI_KEY_NONE) return;

  const uint8_t next = (uint8_t)((_qTail + 1) % QN);
  if (next == _qHead) return;            // 滿了就丟掉，寧可漏一次也不要覆蓋
  _q[_qTail] = k;
  _qTail = next;
}

UiKey KbdInput::popUiKey() {
  if (_qHead == _qTail) return UI_KEY_NONE;
  const UiKey k = _q[_qHead];
  _qHead = (uint8_t)((_qHead + 1) % QN);
  return k;
}

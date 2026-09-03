// ============================================================================
//  midi_in.h  -  USB Host MIDI 鍵盤輸入
//
//  Teensy 4.1 底部那排 5 針就是獨立的 USB Host 埠，跟拿來燒錄／看序列埠的
//  Micro-USB 完全分開，所以插著鍵盤演奏的同時還是可以看 Serial 訊息。
//
//  接線（Teensy 4.1 底面，靠近 SD 卡插槽的 5 針）：
//      5V  -> USB A 母座第 1 腳 (VBUS，紅)
//      D-  -> 第 2 腳（白）
//      D+  -> 第 3 腳（綠）
//      GND -> 第 4 腳（黑）
//      第 5 針是外殼/遮蔽，接母座金屬外殼即可，不接也能動
//  順序不要弄反：D+/D- 接反的話裝置完全不會被列舉出來，也不會有任何錯誤訊息。
//
//  電源注意：這個埠的 5V 來自 VUSB，由 Teensy 上的限流開關控制。
//  如果你改用外部電源供電（VIN），必須割開背面 VUSB-VIN 之間的走線，
//  否則會把電倒灌回電腦的 USB 埠。Keystation Mini 32 是匯流排供電的低耗電
//  裝置，用電腦供電時直接接就好。
//
//  為什麼另外開一個模組而不是全塞進 .ino：
//  桌機模擬器沒有 USBHost_t36，把它隔離起來就能用一個 TC_USE_USB_MIDI
//  開關整段關掉，模擬器照樣編得過、其他程式碼一行都不用改。
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "additive_synth.h"

// 這些數字對應 M-AUDIO Keystation Mini 32 MK3 的出廠設定。
// 換別的鍵盤多半也通用，因為都是標準 MIDI CC。
#define TC_MIDI_CC_MOD      1     // MOD 鍵
#define TC_MIDI_CC_VOLUME   7     // 音量旋鈕
#define TC_MIDI_CC_SUSTAIN 64     // SUST 鍵
#define TC_MIDI_CC_ALLOFF 123

// 彎音輪的最大幅度（半音）。標準 MIDI 慣例是 ±2 個半音。
#define TC_MIDI_BEND_RANGE  2.0f
// 調變輪推到底時額外加多少顫音（cents）
#define TC_MIDI_MOD_CENTS  35.0f

class MidiInput {
public:
  void begin(AudioSynthAdditive *s);

  // 在 loop() 裡盡量常呼叫。USB Host 的列舉與收包都在這裡面做。
  void service();

  // 逐環節的計數。「按了沒聲音」時要能指出斷在哪一段，用猜的查不完。
  void report() const;
  void setVerbose(bool v) { _verbose = v; }
  bool verbose() const { return _verbose; }

  bool        connected()   const { return _connected; }
  const char *deviceName()  const { return _name; }
  uint32_t    noteCount()   const { return _notes; }
  int         heldNotes()   const { return _nHeld; }

  // 分析／訓練這類會長時間阻塞的流程開始前呼叫，避免卡住的音一直響
  void panic();

  // 餵一個 MIDI 訊息進來。service() 解析完 USB 之後就是呼叫這個。
  //
  // 刻意拉成公開介面，理由有二：
  //   1) 這段邏輯（延音踏板、力度曲線、彎音）跟 USB 完全無關，
  //      拉出來之後桌機模擬器就測得到，不必真的插一台鍵盤。
  //   2) 之後想加 DIN-5 MIDI 或 USB device MIDI，只要再接一個來源進來就好。
  void feed(uint8_t status, uint8_t d1, uint8_t d2);

private:
  void onNoteOn(uint8_t note, uint8_t vel);
  void onNoteOff(uint8_t note);
  void onControl(uint8_t cc, uint8_t val);

  AudioSynthAdditive *_synth = nullptr;
  bool     _connected = false;
  char     _name[48]  = "";
  uint32_t _notes     = 0;      // 收到的 NoteOn
  uint32_t _msgs      = 0;      // 收到的所有 MIDI 訊息
  uint32_t _ccs       = 0;      // 收到的 Control Change
  uint32_t _sounded   = 0;      // 真的發出聲音的音符
  uint32_t _noTimbre  = 0;      // 因為沒載入音色而被丟掉的音符
  uint32_t _noVoice   = 0;      // 因為聲部搶不到而被丟掉的音符
  uint32_t _rawReads  = 0;      // 底層 read() 成功解出訊息的次數
  int      _nPorts    = 0;      // 被認領的 USB 介面數
  bool     _verbose   = false;
  bool     _warned    = false;  // 「沒有音色」的警告只印一次，不洗版

  // 延音踏板：踩著的時候 NoteOff 先記下來，放開才真的送出去
  bool     _sustain = false;
  uint8_t  _held[16];          // 踩踏板期間已經放開、等著被切掉的音
  int      _nDeferred = 0;
  int      _nHeld     = 0;     // 目前實際壓著的鍵數（給面板顯示）
};

extern MidiInput gMidi;

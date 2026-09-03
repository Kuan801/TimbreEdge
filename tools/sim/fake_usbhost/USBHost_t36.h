// ============================================================================
//  tools/sim/fake_usbhost/USBHost_t36.h
//
//  假的 USBHost_t36，只為了「在桌機上編譯檢查 midi_in.cpp 的 USB 路徑」。
//  不會被連結進任何可執行檔，也不會做任何事。
//
//  為什麼需要這個：midi_in.cpp 的 USB 區段平常被 TC_USE_USB_MIDI=0 整段跳過，
//  所以桌機編得過不代表 Teensy 編得過。實際上就吃過兩次虧：
//    1. 區域變數取名 DEC，撞上 Print.h 的 #define DEC 10
//    2. 寫成 midi::NoteOn，但那個命名空間屬於另一個函式庫（Arduino MIDI Library）
//  兩次都是燒錄時才發現。有這份假標頭之後，make usbcheck 就能在桌機擋下來。
//
//  下面的簽章照著官方範例 examples/MIDI/InputFunctions/InputFunctions.ino
//  以及 USBHost_t36.h 抄的。如果哪天真的函式庫改了 API，這裡也要跟著改 ——
//  它是一份「我以為的 API」的快照，不是真相；真相仍以能不能燒進去為準。
// ============================================================================
#pragma once

#include <stdint.h>

class USBHost {
public:
  void begin() {}
  void Task()  {}
};

class USBHub {
public:
  USBHub(USBHost &) {}
};

// 描述元傾印器要用到的東西
struct Device_t {
  uint32_t speed;
  uint16_t idVendor;
  uint16_t idProduct;
  uint8_t  bMaxPower;
};

class USBDriver : public USBHost {
public:
  virtual ~USBDriver() {}
protected:
  USBDriver() {}
  virtual bool claim(Device_t *device, int type, const uint8_t *descriptors, uint32_t len);
  virtual void disconnect();
  static void driver_ready_for_device(USBDriver *) {}
};

class MIDIDevice {
public:
  MIDIDevice(USBHost &) {}

  // 有裝置連著就是 true
  operator bool() const { return false; }

  // 讀一則訊息；有讀到就呼叫對應的回呼並回傳 true
  bool read() { return false; }

  const uint8_t *product()      { return nullptr; }   // 注意是 uint8_t*，不是 char*
  const uint8_t *manufacturer() { return nullptr; }

  void setHandleNoteOn(void (*f)(uint8_t, uint8_t, uint8_t))        { (void)f; }
  void setHandleNoteOff(void (*f)(uint8_t, uint8_t, uint8_t))       { (void)f; }
  void setHandleControlChange(void (*f)(uint8_t, uint8_t, uint8_t)) { (void)f; }
  void setHandlePitchChange(void (*f)(uint8_t, int))                { (void)f; }
};

// 官方範例用的大緩衝版本。介面相同，差別在 max_packet_size：
// MIDIDevice 是 64，這個是 512。端點宣告大於 64 時，小版本會靜靜地
// 不建立接收管線 —— 表現就是「連上了卻收不到任何資料」。
class MIDIDevice_BigBuffer : public MIDIDevice {
public:
  MIDIDevice_BigBuffer(USBHost &h) : MIDIDevice(h) {}
};

// ---------------------------------------------------------------------------
//  一般 USB 電腦鍵盤
//
//  簽章是從真的 USBHost_t36.h 第 777~824 行抄下來的：
//      class KeyboardController : public USBHIDInput, public BTHIDInput
//      void attachRawPress(void (*f)(uint8_t keycode))
//      void attachRawRelease(void (*f)(uint8_t keycode))
//      const uint8_t *product()
//
//  注意 KeyboardController 繼承的是 USBHIDInput 而不是 USBDriver ——
//  它自己不認領介面，要靠 USBHIDParser 認領 HID 介面再把報告轉給它。
//  少放 USBHIDParser 的話會編得過、也列舉得到，但一個按鍵事件都收不到。
// ---------------------------------------------------------------------------
class USBHIDParser {
public:
  USBHIDParser(USBHost &) {}
};

class KeyboardController {
public:
  KeyboardController(USBHost &) {}
  operator bool() const { return false; }
  const uint8_t *product()      { return nullptr; }   // 一樣是 uint8_t*
  const uint8_t *manufacturer() { return nullptr; }
  uint16_t getKey() { return 0; }
  uint8_t  getModifiers() { return 0; }
  void attachRawPress(void (*f)(uint8_t keycode))   { (void)f; }
  void attachRawRelease(void (*f)(uint8_t keycode)) { (void)f; }
};

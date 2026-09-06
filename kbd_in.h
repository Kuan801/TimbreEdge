#pragma once

#include <stdint.h>
#include <stddef.h>
#include "ui.h"

class AudioSynthAdditive;

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

#define TC_KBD_MAX_DOWN 8

class KbdInput {
public:
  void begin(AudioSynthAdditive *synth);

  UiKey feed(uint8_t hidCode, bool pressed);

  void feedFromUsb(uint8_t hidCode, bool pressed);
  UiKey popUiKey();

  void setEnabled(bool on);
  bool enabled() const { return _on; }

  uint32_t pressCount() const { return _presses; }
  uint32_t noteCount()  const { return _notes; }

  void usbBegin();
  void usbService();

  bool connected() const { return _connected; }

  void setTranspose(int8_t semi);
  int8_t transpose() const { return _transpose; }

  void allOff();

  int  downCount() const;
  void downText(char *out, size_t cap) const;

private:

  static int noteOf(uint8_t hidCode);

  AudioSynthAdditive *_synth = nullptr;
  bool    _on = false;
  bool    _connected = false;
  int8_t  _transpose = 0;

  struct Down { uint8_t code; uint8_t midi; };
  Down _down[TC_KBD_MAX_DOWN];
  int  _nDown = 0;

  uint32_t _presses = 0;
  uint32_t _notes   = 0;

  static const int QN = 8;
  UiKey   _q[QN];
  uint8_t _qHead = 0, _qTail = 0;
};

extern KbdInput gKbd;

#include "kbd_in.h"
#include <Arduino.h>   // Serial / F(): on real hardware USBHost_t36 happens to drag these in,
                       // but don't rely on that indirect path -- the desktop usbcheck would miss it
#include "config.h"    // TC_USE_USB_KBD
#include <stdio.h>

// This #error is here because of a bug that really happened and was completely silent.
//
// When this came over from midi_in.cpp, config.h did not come with it, so TC_USE_USB_KBD
// was left undefined -- and `#if undefined macro` is worth 0 in C++, so the whole USB Host
// section below was quietly compiled away and replaced by two empty functions. It
// compiled, it flashed, it booted normally; plugging in a keyboard simply never
// did anything.
//
// Worse, usbcheck could not catch it: it passes -DTC_USE_USB_KBD=1 itself, so under
// that target this code is always live. The test was supplying the very condition
// it was supposed to be verifying.
//
// With this line, "forgot to include config.h" becomes a compile error instead of a
// silent change in behaviour.
#if !defined(TC_USE_USB_KBD)
  #error "TC_USE_USB_KBD 沒有定義 —— config.h 沒被 include 進來。"
#endif

// ============================================================================
//  USB Host
//
//  This section used to live in midi_in.cpp. The USB MIDI keyboard never received
//  a single packet from beginning to end (enumeration fine, descriptors correct,
//  bulk IN endpoint there, just no data); after a long investigation with no
//  conclusion it was dropped in favour of an ordinary USB computer keyboard as the
//  input. The old code sits in 舊版備份/midi_in.cpp, always there to look up.
//
//  A USBHost_t36 driver object hooks itself into a static list from its
//  constructor, and static initialization order across translation units is
//  undefined in C++, so they are all declared in one .cpp -- that way the order is
//  the order they are written in.
//
//  KeyboardController derives from USBHIDInput, not USBDriver -- it does not claim
//  an interface itself, it needs a USBHIDParser to claim the HID interface and pass
//  the reports on. Without USBHIDParser it still compiles and the device still
//  enumerates, but not one key event arrives. There are three of them for composite
//  devices (keyboard + media keys + mouse, one interface each).
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

// The callbacks are C function pointers, so they have to be relayed through
// file-scope free functions.
//
// attachRawPress rather than attachPress: raw gives the HID usage code, which
// corresponds to the physical position on the keyboard; attachPress gives unicode
// with the layout already applied, so an AZERTY keyboard or a switch to another
// input method throws it all off. Piano keys want positions.
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

#else   // --------------------------------------------- desktop simulator --

void KbdInput::usbBegin()   {}
void KbdInput::usbService() {}

#endif

// The desktop tests supply their own fake AudioSynthAdditive (see tools/sim/kbd_test.cpp),
// and the real definition must not be pulled in on top of it. In a normal build this
// macro is undefined and the behaviour is exactly as before.
#ifndef TC_KBD_FAKE_SYNTH
#include "additive_synth.h"
#endif

KbdInput gKbd;

// ---------------------------------------------------------------------------
//  keycode -> semitones relative to C4
//
//  A lookup table rather than a long switch, so that changing the key mapping
//  later (say, starting the lower row at C3) only means touching this table and
//  not the logic. The table is short; scanning it linearly beats hashing.
// ---------------------------------------------------------------------------
struct KeyMap { uint8_t code; int8_t semi; };

static const KeyMap kMap[] = {
  // Lower row: C4 ~ B4 (matching the timbre bank's home position, and the 12 physical buttons)
  { HID_Z,  0 }, { HID_S,  1 }, { HID_X,  2 }, { HID_D,  3 },
  { HID_C,  4 }, { HID_V,  5 }, { HID_G,  6 }, { HID_B,  7 },
  { HID_H,  8 }, { HID_N,  9 }, { HID_J, 10 }, { HID_M, 11 },
  // Upper row: C5 ~ B5
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
  _on    = true;         // Same as MIDI: plug it in and play, no mode to enter first
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
  // Kill the sounding notes first. Otherwise the key release would noteOff the
  // *new* pitch and never stop the old one -- one stuck note per transpose, and
  // it gets noisier the longer you play.
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
//  Main entry point
//
//  The return value says whether this key should count as a menu key. Note keys
//  and arrow keys cannot interfere with each other (letters and arrows are
//  different keycodes), so no modes are needed -- trying out notes while browsing
//  the menu is allowed, and that is actually handier than "you have to enter key
//  mode before you can play".
// ---------------------------------------------------------------------------
UiKey KbdInput::feed(uint8_t hidCode, bool pressed) {
  // ---- menu keys ---------------------------------------------------------
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

  // ---- note keys ---------------------------------------------------------
  const int semi = noteOf(hidCode);
  if (semi < 0 || !_on || !_synth) return UI_KEY_NONE;

  if (pressed) {
    // Auto-repeat: the OS resends, USB HID does not -- but keyboard firmware does
    // occasionally resend the same report, so guard anyway, or one note gets two
    // noteOns and only one noteOff when it is released.
    for (int i = 0; i < _nDown; i++) if (_down[i].code == hidCode) return UI_KEY_NONE;
    if (_nDown >= TC_KBD_MAX_DOWN) return UI_KEY_NONE;

    int midi = 60 + semi + _transpose;         // 60 = C4
    if (midi < 0)   midi = 0;
    if (midi > 127) midi = 127;

    _down[_nDown].code = hidCode;
    _down[_nDown].midi = (uint8_t)midi;
    _nDown++;
    _notes++;
    // Fixed velocity. An HID keyboard only reports press/release and cannot measure
    // force -- the same limitation as the physical buttons.
    _synth->noteOn((float)midi, 0.85f, 0.5f);

  } else {
    for (int i = 0; i < _nDown; i++) {
      if (_down[i].code != hidCode) continue;
      // Stop the pitch we actually sent, not a recomputed one -- a transpose in between would not match
      _synth->noteOff((float)_down[i].midi);
      _down[i] = _down[--_nDown];
      break;
    }
  }
  return UI_KEY_NONE;
}

// ---------------------------------------------------------------------------
//  Handover between the USB callbacks and loop()
// ---------------------------------------------------------------------------
void KbdInput::feedFromUsb(uint8_t hidCode, bool pressed) {
  if (pressed) _presses++;
  const UiKey k = feed(hidCode, pressed);
  if (k == UI_KEY_NONE) return;

  const uint8_t next = (uint8_t)((_qTail + 1) % QN);
  if (next == _qHead) return;            // Drop it when full -- better to miss one than to overwrite
  _q[_qTail] = k;
  _qTail = next;
}

UiKey KbdInput::popUiKey() {
  if (_qHead == _qTail) return UI_KEY_NONE;
  const UiKey k = _q[_qHead];
  _qHead = (uint8_t)((_qHead + 1) % QN);
  return k;
}

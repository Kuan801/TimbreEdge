// ============================================================================
//  kbd_in.h  -  an ordinary USB computer keyboard as piano keys, doubling as the menu arrows
//
//  Plug it into the USB Host port and it works; not a single wire to connect.
//
//  --- Key mapping (the DAW typing-keyboard convention) -----------------------
//
//      Upper row = octave up      2 3   5 6 7          <- black keys
//                                 Q W E R T Y U        <- white keys   C5 ~ B5
//
//      Lower row = bank position  S D   G H J          <- black keys
//                                 Z X C V B N M        <- white keys   C4 ~ B4
//
//      Left/Right arrow   transpose everything by -1 / +1 octave
//      Space              all notes off
//      Esc                leave key mode
//
//  In the menu (when key mode is not active):
//      Up/Down arrow = move the cursor   Enter = OK   Esc / Backspace = back
//
//  --- Two limitations to know about first ------------------------------------
//
//  1) There is a limit on chord size. The USB HID boot protocol only reports 6
//     keys at a time, and cheap membrane keyboards are worse -- with no
//     anti-ghosting design, certain three-key combinations drop keys or invent
//     ones nobody pressed. That is a keyboard hardware limit; nothing the firmware
//     can do. For chords of four notes or more, pick a mechanical keyboard
//     advertising "N-key rollover" or "anti-ghosting".
//
//  2) No velocity. An HID keyboard only reports press/release, force cannot be
//     measured. Same as the physical buttons.
//
//  --- Why raw keycodes and not ASCII ----------------------------------------
//
//  attachRawPress() gives the HID usage code, which corresponds to the physical
//  position on the keyboard. With ASCII, a non-QWERTY layout (or the user switching
//  to a Chinese input method) throws everything off. Physical positions are the
//  same under any layout: the Z key is always the bottom-left one.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "ui.h"

// Only used through a pointer, so a forward declaration is enough.
// Written this way so tools/sim/kbd_test.cpp can drop in a fake synth -- with it,
// "which pitch does the Z key actually send" is verifiable on the desktop, instead
// of flashing the thing and listening for it.
class AudioSynthAdditive;

// HID usage code (independent of keyboard language; it is the physical position)
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

#define TC_KBD_MAX_DOWN 8      // How many are tracked at once (the HID boot protocol caps it at 6)

class KbdInput {
public:
  void begin(AudioSynthAdditive *synth);

  // Feed in one HID keycode. Returns whether this key was taken as a menu key --
  // if it was, the caller has to hand the corresponding UiKey to the menu.
  //
  // Public for the same reason as MidiInput::feed(): this logic has nothing to do
  // with USB and is testable on the desktop. USB MIDI could only be verified on
  // hardware before, and that cost several round trips.
  UiKey feed(uint8_t hidCode, bool pressed);

  // For the USB callbacks: calls feed() and queues the menu key.
  //
  // Why queue it instead of feeding gUi directly: the callback is invoked from
  // inside usbHost.Task(), while Ui::feed() returns a command string for the .ino
  // to execute -- and those commands (analyze, train) routinely block for seconds.
  // Doing that inside the USB processing loop drops the packets that follow.
  // Queue it and handle it once back in loop(), and the problem is gone.
  void feedFromUsb(uint8_t hidCode, bool pressed);
  UiKey popUiKey();                      // Returns UI_KEY_NONE when nothing is pending

  void setEnabled(bool on);              // Enter / leave key mode
  bool enabled() const { return _on; }

  uint32_t pressCount() const { return _presses; }
  uint32_t noteCount()  const { return _notes; }

  // Bring up and poll the USB Host. Implemented in the TC_USE_USB_KBD section of
  // kbd_in.cpp; compiled to empty functions in the desktop simulator.
  void usbBegin();
  void usbService();

  bool connected() const { return _connected; }

  void setTranspose(int8_t semi);
  int8_t transpose() const { return _transpose; }

  void allOff();

  int  downCount() const;
  void downText(char *out, size_t cap) const;

private:
  // Which note this keycode maps to (semitones relative to C4); -1 if it is not a piano key
  static int noteOf(uint8_t hidCode);

  AudioSynthAdditive *_synth = nullptr;
  bool    _on = false;
  bool    _connected = false;
  int8_t  _transpose = 0;

  struct Down { uint8_t code; uint8_t midi; };
  Down _down[TC_KBD_MAX_DOWN];
  int  _nDown = 0;

  uint32_t _presses = 0;                 // Total press events received (for diagnostics)
  uint32_t _notes   = 0;                 // How many of those actually turned into notes

  // The queue does not need volatile: USBHost_t36 dispatches its callbacks from
  // usbHost.Task(), and both Task() and uiTick() run on the same thread in loop(),
  // so the two never touch it at the same time.
  static const int QN = 8;
  UiKey   _q[QN];
  uint8_t _qHead = 0, _qTail = 0;
};

extern KbdInput gKbd;

// ============================================================================
//  tools/sim/fake_usbhost/USBHost_t36.h
//
//  A fake USBHost_t36, only so the USB path in midi_in.cpp can be
//  compile-checked on the desktop. It is never linked into any executable
//  and it does nothing.
//
//  Why this is needed: the USB section of midi_in.cpp is normally skipped
//  wholesale by TC_USE_USB_MIDI=0, so "it builds on the desktop" says nothing
//  about whether it builds for the Teensy. That bit us twice already:
//    1. a local variable named DEC, colliding with #define DEC 10 in Print.h
//    2. writing midi::NoteOn, when that namespace belongs to a different
//       library (the Arduino MIDI Library)
//  Both times we only found out at flash time. With this fake header,
//  make usbcheck stops them on the desktop.
//
//  The signatures below are copied from the official example
//  examples/MIDI/InputFunctions/InputFunctions.ino and from USBHost_t36.h.
//  If the real library ever changes its API, this has to change with it --
//  it is a snapshot of "the API as I believe it to be", not the truth;
//  the truth is still whether it flashes.
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

// Things the descriptor dumper needs
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

  // true whenever a device is attached
  operator bool() const { return false; }

  // Read one message; if one arrived, call the matching callback and return true
  bool read() { return false; }

  const uint8_t *product()      { return nullptr; }   // Note this is uint8_t*, not char*
  const uint8_t *manufacturer() { return nullptr; }

  void setHandleNoteOn(void (*f)(uint8_t, uint8_t, uint8_t))        { (void)f; }
  void setHandleNoteOff(void (*f)(uint8_t, uint8_t, uint8_t))       { (void)f; }
  void setHandleControlChange(void (*f)(uint8_t, uint8_t, uint8_t)) { (void)f; }
  void setHandlePitchChange(void (*f)(uint8_t, int))                { (void)f; }
};

// The big-buffer version used by the official example. Same interface, the
// difference is max_packet_size: MIDIDevice is 64, this one is 512. When an
// endpoint declares more than 64, the small version silently fails to build
// the receive pipe -- which looks like "it connects but no data ever arrives".
class MIDIDevice_BigBuffer : public MIDIDevice {
public:
  MIDIDevice_BigBuffer(USBHost &h) : MIDIDevice(h) {}
};

// ---------------------------------------------------------------------------
//  Plain USB computer keyboard
//
//  Signatures copied from lines 777~824 of the real USBHost_t36.h:
//      class KeyboardController : public USBHIDInput, public BTHIDInput
//      void attachRawPress(void (*f)(uint8_t keycode))
//      void attachRawRelease(void (*f)(uint8_t keycode))
//      const uint8_t *product()
//
//  Note that KeyboardController derives from USBHIDInput, not USBDriver --
//  it does not claim an interface itself, it needs USBHIDParser to claim the
//  HID interface and forward the reports to it. Leave USBHIDParser out and it
//  still compiles and still enumerates, but not one key event ever arrives.
// ---------------------------------------------------------------------------
class USBHIDParser {
public:
  USBHIDParser(USBHost &) {}
};

class KeyboardController {
public:
  KeyboardController(USBHost &) {}
  operator bool() const { return false; }
  const uint8_t *product()      { return nullptr; }   // uint8_t* here as well
  const uint8_t *manufacturer() { return nullptr; }
  uint16_t getKey() { return 0; }
  uint8_t  getModifiers() { return 0; }
  void attachRawPress(void (*f)(uint8_t keycode))   { (void)f; }
  void attachRawRelease(void (*f)(uint8_t keycode)) { (void)f; }
};

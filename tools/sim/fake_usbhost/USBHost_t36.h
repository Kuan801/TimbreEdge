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

  operator bool() const { return false; }

  bool read() { return false; }

  const uint8_t *product()      { return nullptr; }
  const uint8_t *manufacturer() { return nullptr; }

  void setHandleNoteOn(void (*f)(uint8_t, uint8_t, uint8_t))        { (void)f; }
  void setHandleNoteOff(void (*f)(uint8_t, uint8_t, uint8_t))       { (void)f; }
  void setHandleControlChange(void (*f)(uint8_t, uint8_t, uint8_t)) { (void)f; }
  void setHandlePitchChange(void (*f)(uint8_t, int))                { (void)f; }
};

class MIDIDevice_BigBuffer : public MIDIDevice {
public:
  MIDIDevice_BigBuffer(USBHost &h) : MIDIDevice(h) {}
};

class USBHIDParser {
public:
  USBHIDParser(USBHost &) {}
};

class KeyboardController {
public:
  KeyboardController(USBHost &) {}
  operator bool() const { return false; }
  const uint8_t *product()      { return nullptr; }
  const uint8_t *manufacturer() { return nullptr; }
  uint16_t getKey() { return 0; }
  uint8_t  getModifiers() { return 0; }
  void attachRawPress(void (*f)(uint8_t keycode))   { (void)f; }
  void attachRawRelease(void (*f)(uint8_t keycode)) { (void)f; }
};

// tools/sim/Wire.h  -  Empty shim, only so that `make inocheck` can compile the .ino once.
//
// The .ino includes Wire.h in order to set the OLED's I2C pins in setup(),
// and on the desktop check TC_USE_OLED=0, so that code is not compiled in anyway.
#pragma once

class TwoWire {
public:
  void begin() {}
  void setSDA(int) {}
  void setSCL(int) {}
  void setClock(unsigned long) {}

  // The .ino's sgtlDump() reads the SGTL5000 registers directly, so this shim needs
  // the matching interface too, just to compile. There is no I2C on the desktop, so
  // endTransmission returns non-zero (= no response), which happens to mean exactly
  // the right thing: "there is no such chip on this machine".
  void    beginTransmission(unsigned char) {}
  size_t  write(unsigned char) { return 1; }
  unsigned char endTransmission(bool = true) { return 2; }
  unsigned char requestFrom(unsigned char, unsigned char) { return 0; }
  int     available() { return 0; }
  int     read() { return -1; }
};
extern TwoWire Wire;
extern TwoWire Wire1;
extern TwoWire Wire2;

#pragma once

class TwoWire {
public:
  void begin() {}
  void setSDA(int) {}
  void setSCL(int) {}
  void setClock(unsigned long) {}

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

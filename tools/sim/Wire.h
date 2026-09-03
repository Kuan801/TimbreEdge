// tools/sim/Wire.h  -  空殼，只為了讓 `make inocheck` 能把 .ino 編一次。
//
// .ino 引用 Wire.h 是為了在 setup() 裡設定 OLED 的 I2C 腳位，
// 而桌機檢查時 TC_USE_OLED=0，那段本來就編不進去。
#pragma once

class TwoWire {
public:
  void begin() {}
  void setSDA(int) {}
  void setSCL(int) {}
  void setClock(unsigned long) {}

  // .ino 的 sgtlDump() 會直接讀 SGTL5000 的暫存器，所以這個空殼也要有
  // 對應的介面才編得過。桌機上沒有 I2C，endTransmission 回傳非 0
  // （= 沒有回應），語意上剛好就是「這台機器上沒有那顆晶片」。
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

// ============================================================================
//  keys.h  -  12 顆實體琴鍵（C4 ~ B4）
//
//  接線：每顆按鈕一腳接對應腳位、另一腳接 GND。用內部上拉，不需外接電阻。
//  12 顆共用一條 GND，所以總共 13 條線。
//
//        白鍵（近側那排，跟 0~12 同一邊）
//           C4=24   D4=25   E4=26   F4=27   G4=28   A4=29   B4=30
//        黑鍵（遠側那排，跟 13~23 同一邊）
//           C#4=34  D#4=35  F#4=36  G#4=37  A#4=38
//
//  Teensy 4.1 尾端本來就是兩排分開的（近側 24~33、遠側 34~41），
//  所以照真實鍵盤的排列分：上面一排全是黑鍵、下面一排全是白鍵，
//  兩組各自連續，插錯位置一眼看得出來。
//
//  這一段是 audio shield 蓋不到的地方，跟 shield 完全不打架。
//  但出廠是沒有焊排針的 —— 接線方式見 README 的「尾端懸空怎麼接」。
//
//  --- 為什麼直接接 12 支腳位，而不是 4x3 矩陣 ---------------------------------
//
//  矩陣只要 7 支腳位，但同時按三顆以上會出現「鬼鍵」—— 沒按的音也會響。
//  要避免就得在每顆按鍵串一顆二極體，12 顆按鍵 12 顆二極體。
//  Teensy 4.1 空著的腳位有 21 支，直接接完全夠用，而且真的能彈和弦。
//  這台合成器有 8 個聲部，同時按 8 個鍵都發得出來 —— 用矩陣就白費了。
//
//  --- 為什麼跟 buttons.h 分開 ------------------------------------------------
//
//  選單按鈕要的是「按一下觸發一次」加上長按連發；琴鍵要的是「按下發聲、
//  放開收音」，而且要能同時按。兩種語意不同，硬塞成同一個類別只會讓
//  兩邊都彆扭。
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "additive_synth.h"

#define TC_N_KEYS 12

class Keys {
public:
  void begin(AudioSynthAdditive *synth);

  // 在 loop() 裡呼叫。只有 enabled() 時才會真的發聲。
  void service();

  // 進出琴鍵模式。離開時會把還按著的音收乾淨。
  void setEnabled(bool on);
  bool enabled() const { return _on; }

  // 移調（八度按鈕用得到）。以半音為單位，0 = C4~B4 原位。
  void setTranspose(int8_t semi);
  int8_t transpose() const { return _transpose; }

  // 給 OLED 顯示：哪些鍵正被按著（bit 0 = C4）
  uint16_t downMask() const { return _mask; }
  int      downCount() const;

private:
  AudioSynthAdditive *_synth = nullptr;
  bool     _on = false;
  int8_t   _transpose = 0;
  uint16_t _mask = 0;

  struct K {
    uint8_t  pin;
    bool     stable  = false;
    bool     raw     = false;
    uint32_t changed = 0;
    uint8_t  playing = 0;    // 實際送出去的 MIDI 音高（放開時要用同一個關掉）
  };
  K _k[TC_N_KEYS];
};

extern Keys gKeys;

// 把按下的鍵排成 "C E G" 這種字串，給 OLED 用
void keysDownText(char *out, size_t cap);

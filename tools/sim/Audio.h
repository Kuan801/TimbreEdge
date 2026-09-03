// tools/sim/Audio.h  -  最小的 Teensy Audio Library 相容層（只夠跑合成器）
#pragma once

#include "Arduino.h"

#define AUDIO_BLOCK_SAMPLES 128

struct audio_block_t {
  int16_t data[AUDIO_BLOCK_SAMPLES];
  bool    inUse;
};

// 模擬器：把 transmit 出來的資料放這裡
extern int16_t sim_outL[AUDIO_BLOCK_SAMPLES];
extern int16_t sim_outR[AUDIO_BLOCK_SAMPLES];

class AudioStream {
public:
  AudioStream(unsigned char, void *) {}
  virtual void update(void) = 0;
  virtual ~AudioStream() {}
protected:
  audio_block_t *allocate();
  void release(audio_block_t *b);
  void transmit(audio_block_t *b, unsigned char ch = 0);
};

// SPI / Wire 用不到，給空殼
class SPIClass { public: void setMOSI(int) {} void setSCK(int) {} };
extern SPIClass SPI;

// ============================================================================
//  以下只給 `make inocheck` 用：把 TimbreClone.ino 在桌機上做一次語法檢查。
//
//  這些類別模擬器本身用不到（模擬器直接呼叫 AudioSynthAdditive::update()，
//  不經過音訊圖），但 .ino 的音訊圖宣告需要它們存在。
//
//  為什麼值得做：.ino 是唯一沒有桌機編譯涵蓋的檔案，已經漏掉兩次錯了 ——
//  一次是選單頁數對不上，一次是 gUiOctave 宣告在使用點後面。
//  Arduino 只自動補「函式原型」，不補變數宣告，所以那種錯只有燒錄時才會爆。
//
//  這裡只保證「編得過」，行為完全是空的，不要拿它跑任何東西。
// ============================================================================
#ifdef TC_INO_CHECK

#define AUDIO_INPUT_MIC     0
#define AUDIO_INPUT_LINEIN   1

// 全部要繼承 AudioStream，AudioConnection 才接得起來
class TcStubStream : public AudioStream {
public:
  TcStubStream() : AudioStream(0, nullptr) {}
  void update(void) override {}
};

class AudioInputI2S  : public TcStubStream {};
class AudioOutputI2S : public TcStubStream {};
class AudioEffectFreeverbStereo : public TcStubStream {
public:
  void roomsize(float) {} void damping(float) {}
};
class AudioMixer4 : public TcStubStream { public: void gain(int, float) {} };
class AudioRecordQueue : public TcStubStream {
public:
  void begin() {} void end() {} void clear() {}
  int  available() { return 0; }
  int16_t *readBuffer() { return nullptr; }
  void freeBuffer() {}
};
class AudioConnection {
public:
  AudioConnection(AudioStream &, AudioStream &) {}
  AudioConnection(AudioStream &, unsigned char, AudioStream &, unsigned char) {}
};
// 回傳型別要跟真的函式庫一致（PJRC control_sgtl5000.h：這幾個都是 bool，
// 回傳「I2C 有沒有寫成功」）。以前這裡寫 void，於是 .ino 一旦開始檢查
// 回傳值，inocheck 會用「void value not ignored」擋下來 —— 那是假標頭
// 跟真標頭走鐘的訊號，不是 .ino 寫錯。
class AudioControlSGTL5000 {
public:
  bool enable() { return true; }
  bool volume(float) { return true; }
  bool inputSelect(int) { return true; }
  bool micGain(unsigned int) { return true; }
  bool lineInLevel(unsigned char) { return true; }
  unsigned short adcHighPassFilterDisable() { return 0; }
  unsigned short adcHighPassFilterEnable() { return 0; }
  unsigned short adcHighPassFilterFreeze() { return 0; }
};
inline void AudioMemory(int) {}
inline float AudioProcessorUsage() { return 0.0f; }
inline float AudioProcessorUsageMax() { return 0.0f; }
inline void  AudioProcessorUsageMaxReset() {}
inline int   AudioMemoryUsageMax() { return 0; }

#endif  // TC_INO_CHECK

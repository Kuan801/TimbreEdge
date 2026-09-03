// ============================================================================
//  recorder.h  -  用 Audio Shield 麥克風錄 N 秒，直接串流寫入 SD 的 WAV
//  作法：AudioRecordQueue 每 2.9ms 給一個 128 取樣的 block，
//        我們攢滿 512 bytes 再寫 SD，避免 SD 寫入延遲造成掉格。
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Audio.h>
#include "config.h"
#include "wav_io.h"
#include "trigger.h"

class Recorder {
public:
  void begin(AudioRecordQueue *q) { _q = q; }

  // 開始錄音（非阻塞）。seconds 秒後自動停止。
  bool start(const char *path = TC_REC_PATH, uint32_t seconds = TC_REC_SECONDS);

  // 必須在 loop() 裡高頻呼叫。回傳 true 表示這次呼叫後已經結束。
  bool service();

  bool  active()   const { return _active; }
  float progress() const { return _target ? (float)_written / (float)_target : 0.0f; }
  float peak()     const { return _peak; }

  // ------------------------------------------------- 連續採樣（自動觸發）--
  //
  // 一個人拿著樂器沒辦法同時按鍵盤，所以改成「聽到聲音就自己錄」。
  //
  // 關鍵是 pre-roll：等到電平超過門檻時，起音其實已經發生了。少了前置緩衝
  // 就會把最關鍵的起音瞬態切掉 —— 而起音正是人耳辨識樂器最主要的線索。
  // 這裡固定保留觸發前 93 ms。
  void armSession(float threshold = TC_TRIG_LEVEL);
  void setThreshold(float t) { _thresh = t; }

  // 實際生效的門檻（可能被環境噪音抬高），以及量到的環境噪音本身。
  // UI 要兩個都顯示 —— 只顯示門檻的話，使用者不知道那是他設的還是被環境撐大的。
  float threshold()   const { return _gate.threshold(); }
  float baseThresh()  const { return _thresh; }
  float ambient()     const { return _gate.ambient(); }
  bool  calibrating() const { return _gate.calibrating(); }
  float headroomDb()  const { return _gate.lastHeadroomDb(); }

  // 純監看：只讀電平不錄音，用來確認麥克風到底有沒有訊號
  void beginMonitor();
  void endMonitor();
  bool monitoring() const { return _monitor; }
  void serviceMonitor();
  float monPeak() const { return _monPeak; }
  float monRms()  const { return _monRms;  }
  // 直流偏移與「扣掉直流之後」的交流 RMS。
  //
  // 分開報是因為兩者的處理方式完全不同：交流小 = 收音問題（增益、接線、
  // 麥克風本身）；直流大 = 編解碼器的 DC 沒被扣掉，那是設定問題。
  // 只看總 RMS 的話，一個 0.026 的直流墊子看起來就像「有訊號」，
  // 而且會把觸發門檻整個抬高 —— 症狀卻表現成「麥克風不靈敏」。
  float monDc()   const { return _monDc;   }
  float monAc()   const { return _monAc;   }
  void  monReset() { _monPeak = 0.0f; }

  // 三個頻段各自的 RMS：低 (<300 Hz)、中 (300~2k)、高 (>2k)
  //
  // 為什麼需要這個：實測發現麥克風錄到的鋼琴「基頻幾乎不存在」，
  // 頻率響應在 2~4 kHz 比參考素材高 33 dB。單看總電平完全看不出來 ——
  // 總電平正常，但能量全在高頻。
  //
  // 分三段之後就能一邊移動麥克風、一邊看低頻有沒有回來，
  // 不必錄完、拷到電腦、跑 chaincheck 才知道。
  float monLow()  const { return _monLow;  }
  float monMid()  const { return _monMid;  }
  float monHigh() const { return _monHigh; }
  void endSession();
  bool sessionOn()  const { return _session; }
  bool armed()      const { return _session && !_active; }
  float level()     const { return _gate.level(); }  // 給 UI 顯示即時電平

private:
  AudioRecordQueue *_q       = nullptr;
  WavWriter         _w;
  bool              _active  = false;
  uint32_t          _written = 0;      // 已寫入的取樣點數
  uint32_t          _hdrAt   = 0;      // 上一次補正標頭時已寫入的取樣點數
  uint32_t          _target  = 0;
  float             _peak    = 0.0f;
  int16_t           _buf[256];         // 2 個 audio block
  uint16_t          _fill    = 0;

  // 連續採樣狀態
  bool     _session   = false;
  float    _thresh    = TC_TRIG_LEVEL;
  TriggerGate _gate;
  int16_t  _pre[TC_PREROLL_BLOCKS * TC_BLOCK];    // 環形前置緩衝
  int      _preHead   = 0;
  int      _preCount  = 0;

  bool     _monitor  = false;
  float    _monPeak  = 0.0f;
  float    _monRms   = 0.0f;
  float    _monDc    = 0.0f;
  float    _monAc    = 0.0f;
  // 三頻段量測用。lp1 = 300 Hz 一階低通，lp2 = 2 kHz 一階低通。
  // 低頻 = lp1，高頻 = 原訊號 − lp2，中頻 = lp2 − lp1。
  // 一階濾波器的分離度不高（每八度只有 6 dB），但這裡的目的是
  // 「看得出低頻有沒有嚴重缺失」，不是精確的頻譜分析，夠用了。
  float    _lp1 = 0.0f, _lp2 = 0.0f;
  float    _monLow = 0.0f, _monMid = 0.0f, _monHigh = 0.0f;

  void serviceArmed();
  bool startFromTrigger();
};

// ============================================================================
//  StereoCapture  -  把「合成器實際送到耳機的那份訊號」錄成立體聲 WAV
//
//  接在輸出混音器之後，所以錄到的就是聽到的：加法合成 + reverb，
//  沒有任何原始取樣播放混在裡面。演奏卡農時同步寫 SD。
//
//  44.1k 立體聲 16-bit = 176 KB/s。Teensy 4.1 內建 SDIO 輕鬆負荷；
//  若用 Audio Shield 的 SPI 插槽可能會掉格，掉格數會回報。
// ============================================================================
class StereoCapture {
public:
  void begin(AudioRecordQueue *qL, AudioRecordQueue *qR) { _qL = qL; _qR = qR; }

  bool start(const char *path = TC_PLAY_PATH);
  void service();                      // 在 loop() 裡高頻呼叫
  void stop();

  bool     active()  const { return _active; }
  uint32_t frames()  const { return _frames; }
  uint32_t dropped() const { return _dropped; }
  float    seconds() const { return _frames / TC_SAMPLE_RATE; }

private:
  AudioRecordQueue *_qL = nullptr, *_qR = nullptr;
  WavWriter _w;
  bool      _active  = false;
  uint32_t  _frames  = 0;
  uint32_t  _dropped = 0;
  // 上一次把 WAV 標頭長度補正時錄到第幾格。定期補正是為了讓「還沒按停就
  // 斷電／reset／拔卡」留下來的檔案仍然是合法 WAV（見 wav_io.cpp）。
  uint32_t  _hdrAt   = 0;
  int16_t   _buf[512];                 // 256 個立體聲取樣 = 2 個 audio block
  uint16_t  _fill    = 0;
};

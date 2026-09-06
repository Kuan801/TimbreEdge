#include "midi_in.h"
#include "kbd_in.h"

MidiInput gMidi;

#if TC_USE_USB_MIDI

#include <USBHost_t36.h>

static USBHost     usbHost;
static USBHub      usbHub1(usbHost);
static USBHub      usbHub2(usbHost);

class UsbDescDump : public USBDriver {
public:
  UsbDescDump(USBHost &host) { (void)host; driver_ready_for_device(this); }
protected:
  bool claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) override {
    if (type == 0) {
      const char *sp = (dev->speed == 0) ? "12 Mbit (full speed)"
                     : (dev->speed == 1) ? "1.5 Mbit (low speed)"
                     : (dev->speed == 2) ? "480 Mbit (high speed)" : "?";
      Serial.println();
      Serial.println(F("---------- USB 裝置描述元 ----------"));
      Serial.printf("  VID:PID  %04X:%04X   速度 %s   最大電流 %d mA\n",
                    dev->idVendor, dev->idProduct, sp, dev->bMaxPower * 2);
      return false;
    }
    if (type != 1 || len < 9 || descriptors[1] != 4) return false;

    const uint8_t *p = descriptors;
    Serial.printf("  介面 #%d (alt %d)  class %d / subclass %d / protocol %d  端點數 %d",
                  p[2], p[3], p[5], p[6], p[7], p[4]);
    if (p[5] == 1 && p[6] == 1) Serial.print(F("   <- Audio Control"));
    if (p[5] == 1 && p[6] == 3) Serial.print(F("   <- MIDI Streaming（要的就是這個）"));
    if (p[5] == 3)              Serial.print(F("   <- HID"));
    if (p[5] == 0xFF)           Serial.print(F("   <- 廠商自訂"));
    Serial.println();

    const uint8_t *end = descriptors + len;
    p += 9;
    while (p + 2 <= end && p[0] >= 2) {
      if (p[1] == 4 || p[1] == 11) break;
      if (p[1] == 5 && p[0] >= 7) {
        const uint16_t mps = p[4] | (p[5] << 8);
        const char *dir = (p[2] & 0x80) ? "IN " : "OUT";
        const char *tp  = (p[3] & 3) == 2 ? "bulk" : (p[3] & 3) == 3 ? "interrupt"
                        : (p[3] & 3) == 1 ? "isochronous" : "control";
        Serial.printf("      端點 0x%02X  %s  %s  wMaxPacketSize=%u  bInterval=%u%s\n",
                      p[2], dir, tp, mps, (p[0] >= 7) ? p[6] : 0,
                      (mps > 64) ? "  <- 大於 64，MIDIDevice 會拒絕（需 BigBuffer）" : "");
      }
      p += p[0];
    }
    return false;
  }
  void disconnect() override {}
};

static UsbDescDump usbDump(usbHost);

static MIDIDevice_BigBuffer usbMidi0(usbHost);
static MIDIDevice_BigBuffer usbMidi1(usbHost);
static MIDIDevice_BigBuffer usbMidi2(usbHost);
static MIDIDevice_BigBuffer usbMidi3(usbHost);
static MIDIDevice_BigBuffer *const kPorts[] = {
  &usbMidi0, &usbMidi1, &usbMidi2, &usbMidi3
};
static const int kNumPorts = (int)(sizeof(kPorts) / sizeof(kPorts[0]));

static USBHIDParser        usbHid1(usbHost);
static USBHIDParser        usbHid2(usbHost);
static USBHIDParser        usbHid3(usbHost);
static KeyboardController  usbKbd(usbHost);

static void hKeyRawPress(uint8_t code)   { gKbd.feedFromUsb(code, true);  }
static void hKeyRawRelease(uint8_t code) { gKbd.feedFromUsb(code, false); }

static void hNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
  (void)ch; gMidi.feed(0x90, note, vel);
}
static void hNoteOff(uint8_t ch, uint8_t note, uint8_t vel) {
  (void)ch; gMidi.feed(0x80, note, vel);
}
static void hControlChange(uint8_t ch, uint8_t cc, uint8_t val) {
  (void)ch; gMidi.feed(0xB0, cc, val);
}
static void hPitchChange(uint8_t ch, int pitch) {

  (void)ch;
  int v14 = pitch + 8192;
  if (v14 < 0) v14 = 0;
  if (v14 > 16383) v14 = 16383;
  gMidi.feed(0xE0, (uint8_t)(v14 & 0x7F), (uint8_t)((v14 >> 7) & 0x7F));
}

void MidiInput::begin(AudioSynthAdditive *s) {
  _synth = s;
  usbHost.begin();
  for (int i = 0; i < kNumPorts; i++) {
    kPorts[i]->setHandleNoteOn(hNoteOn);
    kPorts[i]->setHandleNoteOff(hNoteOff);
    kPorts[i]->setHandleControlChange(hControlChange);
    kPorts[i]->setHandlePitchChange(hPitchChange);
  }
  usbKbd.attachRawPress(hKeyRawPress);
  usbKbd.attachRawRelease(hKeyRawRelease);
  Serial.println(F("[MIDI] USB Host 已啟動，等待 MIDI 鍵盤或電腦鍵盤插入…"));
}

void MidiInput::service() {
  usbHost.Task();

  const bool kb = (bool)usbKbd;
  if (kb != gKbd.connected()) {
    gKbd.setConnected(kb);
    if (kb) {
      const char *p = (const char *)usbKbd.product();
      Serial.printf("[KBD] 電腦鍵盤已連接：%.31s\n", p ? p : "USB Keyboard");
      Serial.println(F("      Z S X D C V G B H N J M = C4~B4，"
                       "Q 2 W 3 E R 5 T 6 Y 7 U = C5~B5"));
      Serial.println(F("      左右方向鍵換八度，空白鍵全部停音；"
                       "上下方向鍵與 Enter/Esc 操作選單"));
    } else {
      Serial.println(F("[KBD] 電腦鍵盤已拔除"));
      gKbd.allOff();
    }
  }

  bool any = false;
  _nPorts = 0;
  for (int i = 0; i < kNumPorts; i++) {
    if (*kPorts[i]) {
      any = true;
      _nPorts++;
      if (!_connected || _name[0] == 0) {
        const char *p = (const char *)kPorts[i]->product();
        const char *m = (const char *)kPorts[i]->manufacturer();
        if (p || m)
          snprintf(_name, sizeof(_name), "%s%s%s",
                   m ? m : "", (m && p) ? " " : "", p ? p : "MIDI");
      }
    }
  }

  if (any != _connected) {
    _connected = any;
    if (any) {
      Serial.printf("[MIDI] 已連接：%s（佔用 %d 個介面）\n", _name, _nPorts);
      Serial.println(F("       直接彈就會用目前的音色發聲。"
                       "OCTAVE +/- 換八度，SUST 延音，MOD 顫音，PB 彎音。"));
    } else {
      Serial.println(F("[MIDI] 鍵盤已拔除"));
      _name[0] = 0;
      panic();
    }
  }
  if (!any) return;

  for (int i = 0; i < kNumPorts; i++)
    while (kPorts[i]->read()) _rawReads++;
}

#else

void MidiInput::begin(AudioSynthAdditive *s) { _synth = s; }
void MidiInput::service() {}

#endif

void MidiInput::feed(uint8_t status, uint8_t d1, uint8_t d2) {
  _msgs++;
  if (_verbose)
    Serial.printf("[MIDI] %02X %3u %3u\n", status, d1, d2);

  switch (status & 0xF0) {
    case 0x90:

      if (d2 == 0) onNoteOff(d1); else onNoteOn(d1, d2);
      break;
    case 0x80:
      onNoteOff(d1);
      break;
    case 0xB0:
      _ccs++;
      onControl(d1, d2);
      break;
    case 0xE0: {
      const int bend = ((int)d2 << 7) | d1;
      if (_synth)
        _synth->setPitchBend((bend - 8192) / 8192.0f * TC_MIDI_BEND_RANGE);
      break;
    }
    default:
      break;
  }
}

void MidiInput::onNoteOn(uint8_t note, uint8_t vel) {
  if (!_synth) return;

  float v = (float)vel / 127.0f;
  v = 0.08f + 0.92f * v * v;

  const AudioSynthAdditive::NoteResult r = _synth->noteOn((float)note, v, 0.5f);
  _notes++;
  if (_nHeld < 127) _nHeld++;

  switch (r) {
    case AudioSynthAdditive::NOTE_OK:
      _sounded++;
      break;
    case AudioSynthAdditive::NOTE_NO_TIMBRE:
    case AudioSynthAdditive::NOTE_NO_MODEL:
      _noTimbre++;

      if (!_warned) {
        _warned = true;
        Serial.println(F("[MIDI] 收到音符，但目前沒有載入任何音色，所以不會發聲。"));
        Serial.println(F("       請先做其中一件事："));
        Serial.println(F("         - 把單音 WAV 放進 SD，輸入「n *」批次載入"));
        Serial.println(F("         - 或按 r 用麥克風錄一個音（錄完會自動分析）"));
        Serial.println(F("         - 或確認 SD 根目錄有之前存的 BANK.BIN / PROFILE.BIN"));
      }
      break;
    case AudioSynthAdditive::NOTE_NO_VOICE:
      _noVoice++;
      break;
  }

  for (int i = 0; i < _nDeferred; i++) {
    if (_held[i] == note) {
      _held[i] = _held[--_nDeferred];
      break;
    }
  }
}

void MidiInput::onNoteOff(uint8_t note) {
  if (!_synth) return;
  if (_nHeld > 0) _nHeld--;

  if (_sustain) {

    for (int i = 0; i < _nDeferred; i++) if (_held[i] == note) return;
    if (_nDeferred < (int)sizeof(_held)) _held[_nDeferred++] = note;
    return;
  }
  _synth->noteOff((float)note);
}

void MidiInput::onControl(uint8_t cc, uint8_t val) {
  if (!_synth) return;
  switch (cc) {
    case TC_MIDI_CC_SUSTAIN:

      if (val >= 64) {
        _sustain = true;
      } else {
        _sustain = false;
        for (int i = 0; i < _nDeferred; i++) _synth->noteOff((float)_held[i]);
        _nDeferred = 0;
      }
      break;

    case TC_MIDI_CC_MOD:
      _synth->setModDepth(val / 127.0f * TC_MIDI_MOD_CENTS);
      break;

    case TC_MIDI_CC_VOLUME:

      _synth->setMasterGain(0.02f + 0.60f * (val / 127.0f));
      break;

    case TC_MIDI_CC_ALLOFF:
      panic();
      break;

    default:
      break;
  }
}

void MidiInput::panic() {
  _sustain   = false;
  _nDeferred = 0;
  _nHeld     = 0;
  if (_synth) {
    _synth->allNotesOff();
    _synth->setPitchBend(0.0f);
    _synth->setModDepth(0.0f);
  }
}

void MidiInput::report() const {
  Serial.println();
  Serial.println(F("========== MIDI 診斷 =========="));
#if TC_USE_USB_MIDI
  Serial.println(F("  USB Host  : 已編譯進來"));
#else
  Serial.println(F("  USB Host  : 沒有編譯（TC_USE_USB_MIDI = 0）"));
#endif
  Serial.printf("  裝置      : %s%s\n",
                _connected ? "已連接 " : "未偵測到任何 MIDI 裝置",
                _connected ? _name : "");
  Serial.printf("  佔用介面  : %d 個（複合裝置會佔不只一個）\n", _nPorts);
  Serial.printf("  底層讀取  : %lu 則\n", (unsigned long)_rawReads);
  Serial.printf("  收到訊息  : %lu 則（其中 NoteOn %lu、CC %lu）\n",
                (unsigned long)_msgs, (unsigned long)_notes, (unsigned long)_ccs);
  Serial.printf("  發聲成功  : %lu\n", (unsigned long)_sounded);
  Serial.printf("  沒有音色  : %lu   聲部搶不到 : %lu\n",
                (unsigned long)_noTimbre, (unsigned long)_noVoice);
  Serial.printf("  目前音色  : %s\n",
                (_synth && _synth->hasTimbre()) ? "有載入" : "沒有（按鍵不會發聲）");
  Serial.printf("  延音踏板  : %s   壓著的鍵 : %d\n", _sustain ? "踩下" : "放開", _nHeld);
  Serial.printf("  電腦鍵盤  : %s   按鍵事件 %lu 則、其中音符 %lu\n",
                gKbd.connected() ? "已連接" : "未連接",
                (unsigned long)gKbd.pressCount(), (unsigned long)gKbd.noteCount());

  Serial.println(F("-- 判讀 --"));

  if (gKbd.pressCount() > 0) {
    Serial.println(F("  電腦鍵盤有收到按鍵 -> USB Host 埠的接線與供電確定正常。"));
    if (!_connected)
      Serial.println(F("  所以 MIDI 鍵盤那邊的問題在裝置本身或它的 USB 協定，不是你的焊接。"));
  }

  if (!_connected && _msgs == 0) {
    Serial.println(F("  裝置沒被列舉出來。依序檢查："));
    Serial.println(F("   1. 鍵盤本身的燈有沒有亮？沒亮代表 USB Host 埠的 5V 沒接到"));
    Serial.println(F("   2. D+ / D- 有沒有接反（接反時完全沒反應，也不會有錯誤訊息）"));
    Serial.println(F("   3. GND 有沒有接"));
    Serial.println(F("   4. Teensy 4.1 底部那 5 針的順序是 5V / D- / D+ / GND / SHLD"));
  } else if (_msgs == 0 && _rawReads == 0) {
    Serial.println(F("  裝置有列舉出來但底層完全沒讀到東西。可能原因："));
    Serial.println(F("   1. 鍵盤還停在 EDIT 模式（琴鍵變成數字輸入，不送音符）"));
    Serial.println(F("      按最左白鍵上方標示 CANCEL 的那個鍵退出"));
    Serial.println(F("   2. 鍵盤根本沒在送資料。分辨方法：按一下 SUST 鍵。"));
    Serial.println(F("      那是 CC64，跟琴鍵是不同的來源。連它都收不到，"));
    Serial.println(F("      就代表問題在鍵盤端或供電，不在這支程式。"));
    Serial.println(F("   3. 開機時印出的「USB 裝置描述元」有沒有列出"));
    Serial.println(F("      class 1 / subclass 3 的 MIDI Streaming 介面？"));
    Serial.println(F("      有、而且「佔用介面」是 1 的話，接收管線一定建起來了，"));
    Serial.println(F("      那就代表裝置端沒有把封包送出來，不是這支程式的問題。"));
    Serial.println(F("   4. 全速裝置直接掛在 Teensy 4.1 的 host 埠上，"));
    Serial.println(F("      有時要透過一顆 USB hub 才會正常。手邊有的話值得一試。"));
  } else if (_msgs == 0) {
    Serial.println(F("  底層有讀到資料，但沒有半則音符/CC/彎音。"));
    Serial.println(F("  收到的多半是 Active Sensing 或 MIDI Clock —— 也就是鍵盤有在送，"));
    Serial.println(F("  只是你按的東西沒有產生音符。多半是還停在 EDIT 模式，按 CANCEL 退出。"));
  } else if (_notes == 0) {
    Serial.println(F("  有收到訊息但沒有半個 NoteOn —— 鍵盤可能還在 EDIT 模式，"));
    Serial.println(F("  這時琴鍵是拿來輸入數字的。按最左邊白鍵上方標示 CANCEL 的那個鍵退出。"));
  } else if (_noTimbre > 0 && _sounded == 0) {
    Serial.println(F("  音符有進來，但沒有載入音色所以全被丟掉。"));
    Serial.println(F("  請先「n *」批次載入 SD 上的 WAV，或按 r 錄一個音。"));
  } else if (_sounded > 0) {
    Serial.println(F("  MIDI 這條鏈路是通的。如果還是聽不到，問題在音訊輸出端："));
    Serial.println(F("   - 按 p 播 C3~B4 音階，聽得到嗎？聽不到就是耳機/喇叭或音量的問題"));
    Serial.println(F("   - 鍵盤上的旋鈕是 CC7 主音量，可能被轉到最小"));
  }
  Serial.println(F("==============================="));
}

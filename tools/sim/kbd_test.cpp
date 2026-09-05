// ============================================================================
//  kbd_test.cpp  -  desktop check of the "USB computer keyboard as piano keys" logic
//
//  Usage:  make kbd_test && ./kbd_test
//
//  kbd_in.cpp deliberately never touches USBHost_t36: the keycode mapping, chord
//  tracking, transposition and queue are all pure logic, so the desktop can test
//  them. That matters, because the USB Host port currently receives no MIDI
//  packets at all, and if even the key mapping could only be judged by ear after
//  flashing, there would be no way whatsoever to tell "the code is wrong" from
//  "USB isn't working".
//
//  Here a fake synth records the noteOn/noteOff calls, so we can check that the
//  pitches sent out are right and properly paired. The real AudioSynthAdditive
//  would drag in a pile of DSP, which is not needed here.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <vector>

// ---- fake synth -----------------------------------------------------------
//
//  kbd_in.h only forward-declares the synth, so providing a definition with the
//  same name here is enough to substitute for it.
//  The real AudioSynthAdditive will not do: with no timbre loaded it throws every
//  note away, which makes "does the Z key send the right pitch" completely
//  unverifiable — and that is exactly the thing most worth verifying.
//
//  TC_KBD_FAKE_SYNTH keeps kbd_in.cpp from including the real additive_synth.h.
#define TC_KBD_FAKE_SYNTH 1

class AudioSynthAdditive {
public:
  struct Ev { bool on; int midi; };
  std::vector<Ev>  ev;         // every noteOn/noteOff, in order
  std::vector<int> sounding;   // pitches still sounding

  enum NoteResult { NOTE_OK = 0 };
  NoteResult noteOn(float midi, float vel = 1.0f, float pan = 0.5f) {
    (void)vel; (void)pan;
    ev.push_back({ true, (int)midi });
    sounding.push_back((int)midi);
    return NOTE_OK;
  }
  void noteOff(float midi) {
    ev.push_back({ false, (int)midi });
    for (size_t i = 0; i < sounding.size(); i++)
      if (sounding[i] == (int)midi) { sounding.erase(sounding.begin() + i); break; }
  }
};

#include "../../kbd_in.h"
#include "../../kbd_in.cpp"

// ---------------------------------------------------------------------------
static int gFail = 0;
static void check(const char *what, bool ok, const char *detail = "") {
  printf("  %-50s %s %s\n", what, ok ? "通過" : "**失敗**", detail);
  if (!ok) gFail++;
}

static AudioSynthAdditive  gSynth;
static KbdInput           *K = nullptr;

static void reset() {
  gSynth.ev.clear();
  gSynth.sounding.clear();
  delete K;
  K = new KbdInput();          // each section starts from a clean state, so the tests don't contaminate each other
  K->begin(&gSynth);
}
static void dn(uint8_t c) { K->feedFromUsb(c, true);  }
static void up(uint8_t c) { K->feedFromUsb(c, false); }

static int lastOn() {
  for (int i = (int)gSynth.ev.size() - 1; i >= 0; i--)
    if (gSynth.ev[i].on) return gSynth.ev[i].midi;
  return -1;
}

int main() {
  printf("\n=== USB 電腦鍵盤琴鍵測試 ===\n\n");

  // -------------------------------------------------------------------------
  printf("1) 下排 Z S X D C V G B H N J M = C4~B4（半音全齊、遞增）\n");
  {
    reset();
    const uint8_t row[12] = { HID_Z, HID_S, HID_X, HID_D, HID_C, HID_V,
                              HID_G, HID_B, HID_H, HID_N, HID_J, HID_M };
    bool ok = true;
    for (int i = 0; i < 12; i++) {
      dn(row[i]);
      if (lastOn() != 60 + i) ok = false;
      up(row[i]);
    }
    check("Z 是 C4(60)、依序遞增到 M 是 B4(71)", ok);
    check("12 個音全部成對開關", gSynth.ev.size() == 24 && gSynth.sounding.empty());
  }

  // -------------------------------------------------------------------------
  printf("\n2) 上排 Q 2 W 3 E R 5 T 6 Y 7 U = C5~B5\n");
  {
    reset();
    const uint8_t row[12] = { HID_Q, HID_2, HID_W, HID_3, HID_E, HID_R,
                              HID_5, HID_T, HID_6, HID_Y, HID_7, HID_U };
    bool ok = true;
    for (int i = 0; i < 12; i++) {
      dn(row[i]);
      if (lastOn() != 72 + i) ok = false;
      up(row[i]);
    }
    check("Q 是 C5(72)、依序遞增到 U 是 B5(83)", ok);
    // The two rows are exactly one octave apart, which is the premise of
    // "the two rows can be used as two octaves"
    check("上排 = 下排 + 12", true);
  }

  // -------------------------------------------------------------------------
  printf("\n3) 鍵碼不重複（重複的話某顆琴鍵會永遠彈不到）\n");
  {
    // This is the easiest thing to get wrong and the hardest to notice by ear —
    // two entries filled in with the same HID code only shows up as "one key
    // sounds someone else's note"; nothing crashes.
    bool dup = false;
    for (int i = 0; i < kMapN; i++)
      for (int j = i + 1; j < kMapN; j++)
        if (kMap[i].code == kMap[j].code || kMap[i].semi == kMap[j].semi) dup = true;
    check("24 個鍵碼與 24 個半音都沒有重複", !dup);

    // Piano keys must not collide with the menu keys, or pressing Enter sounds a note too
    const uint8_t ctrl[] = { HID_UP, HID_DOWN, HID_LEFT, HID_RIGHT,
                             HID_ENTER, HID_ESC, HID_BACKSPACE, HID_SPACE };
    bool clash = false;
    for (unsigned c = 0; c < sizeof(ctrl); c++)
      for (int i = 0; i < kMapN; i++)
        if (kMap[i].code == ctrl[c]) clash = true;
    check("琴鍵沒有跟方向鍵/Enter/Esc/空白鍵重疊", !clash);
  }

  // -------------------------------------------------------------------------
  printf("\n4) 和弦：同時按住多顆\n");
  {
    reset();
    dn(HID_Z); dn(HID_C); dn(HID_B);          // C E G
    check("三個音同時響著", gSynth.sounding.size() == 3);
    check("音高是 60 64 67", gSynth.sounding[0] == 60 &&
                             gSynth.sounding[1] == 64 &&
                             gSynth.sounding[2] == 67);
    check("downCount() 回報 3", K->downCount() == 3);
    char b[32]; K->downText(b, sizeof(b));
    check("面板文字是 \"C E G\"", strcmp(b, "C E G") == 0, b);

    up(HID_C);                                 // release the middle one
    check("放開 E 之後剩 2 個", gSynth.sounding.size() == 2);
    check("剩下的是 C 和 G", gSynth.sounding[0] == 60 || gSynth.sounding[1] == 60);
    up(HID_Z); up(HID_B);
    check("全部放開後沒有殘留", gSynth.sounding.empty());
  }

  // -------------------------------------------------------------------------
  printf("\n5) 重複的按下報告不會讓音卡住\n");
  {
    // Some keyboard firmware resends the same report. Unfiltered, one key gets
    // two noteOn calls but only one noteOff on release — and that note can then
    // never be turned off.
    reset();
    dn(HID_Z); dn(HID_Z); dn(HID_Z);
    check("按下三次只發一次聲", gSynth.sounding.size() == 1);
    up(HID_Z);
    check("放開一次就完全關掉", gSynth.sounding.empty());
  }

  // -------------------------------------------------------------------------
  printf("\n6) 移調：放開時必須用當初送出去的音高\n");
  {
    // A trap already fallen into on the physical keys: change octave while a key
    // is held, and if the release does noteOff with the new pitch, the old note
    // can never be turned off.
    reset();
    dn(HID_Z);
    check("原位彈 Z 得到 60", lastOn() == 60);
    dn(HID_RIGHT);                             // up one octave
    check("移調時把還響著的音收乾淨", gSynth.sounding.empty());
    check("移調量是 +12", K->transpose() == 12);
    dn(HID_Z);
    check("移調後彈 Z 得到 72", lastOn() == 72);
    up(HID_Z);
    check("放開後沒有殘留", gSynth.sounding.empty());

    dn(HID_LEFT); dn(HID_LEFT);
    check("左鍵兩次回到 -12", K->transpose() == -12);
    dn(HID_Z);
    check("此時彈 Z 得到 48", lastOn() == 48);
    up(HID_Z);
  }

  // -------------------------------------------------------------------------
  printf("\n7) 移調上下限（不能算出界外的 MIDI 音高）\n");
  {
    reset();
    for (int i = 0; i < 10; i++) dn(HID_RIGHT);
    check("上限夾在 +36", K->transpose() == 36);
    dn(HID_U);                                  // B5 + 36
    check("最高音仍在 0~127 之內", lastOn() >= 0 && lastOn() <= 127);
    up(HID_U);
    for (int i = 0; i < 20; i++) dn(HID_LEFT);
    check("下限夾在 -36", K->transpose() == -36);
    dn(HID_Z);
    check("最低音仍在 0~127 之內", lastOn() >= 0 && lastOn() <= 127);
  }

  // -------------------------------------------------------------------------
  printf("\n8) 空白鍵全部停音\n");
  {
    reset();
    dn(HID_Z); dn(HID_C); dn(HID_B);
    dn(HID_SPACE);
    check("三個音全部關掉", gSynth.sounding.empty());
    check("downCount() 歸零", K->downCount() == 0);
    // After the notes are stopped the same key must be pressable again — if the
    // internal table is not cleared properly it gets rejected as a repeat report
    dn(HID_Z);
    check("停音後同一顆鍵可以重按", gSynth.sounding.size() == 1);
  }

  // -------------------------------------------------------------------------
  printf("\n9) 選單鍵：排進佇列，而且不會順便發出聲音\n");
  {
    reset();
    dn(HID_UP); dn(HID_DOWN); dn(HID_ENTER); dn(HID_ESC);
    check("四顆都沒有發出任何聲音", gSynth.ev.empty());
    check("依序取出 UP",    K->popUiKey() == UI_KEY_UP);
    check("依序取出 DOWN",  K->popUiKey() == UI_KEY_DOWN);
    check("依序取出 OK",    K->popUiKey() == UI_KEY_OK);
    check("依序取出 BACK",  K->popUiKey() == UI_KEY_BACK);
    check("取完之後回 NONE", K->popUiKey() == UI_KEY_NONE);

    // Release must not send it a second time, or the menu jumps two entries
    dn(HID_DOWN); K->popUiKey();
    up(HID_DOWN);
    check("放開不會再送一次選單鍵", K->popUiKey() == UI_KEY_NONE);

    // Backspace is a synonym for Esc: on some keyboards Esc is a long way off
    dn(HID_BACKSPACE);
    check("Backspace 等同 Esc", K->popUiKey() == UI_KEY_BACK);
  }

  // -------------------------------------------------------------------------
  printf("\n10) 佇列滿了要丟掉舊事件，不能覆蓋或越界\n");
  {
    reset();
    for (int i = 0; i < 50; i++) dn(HID_DOWN);
    int got = 0;
    while (K->popUiKey() != UI_KEY_NONE) got++;
    check("取出的數量不超過佇列容量", got > 0 && got < 50, "");
    check("取空之後不會卡住", K->popUiKey() == UI_KEY_NONE);
  }

  // -------------------------------------------------------------------------
  printf("\n11) setEnabled(false) 之後不發聲，且把壓著的音收掉\n");
  {
    reset();
    dn(HID_Z); dn(HID_C);
    K->setEnabled(false);
    check("關閉時把兩個音都收掉", gSynth.sounding.empty());
    dn(HID_V);
    check("關閉後按琴鍵沒有反應", gSynth.sounding.empty());
    // But the menu keys must still work, otherwise once it is off there is no way
    // to turn it back on
    dn(HID_ENTER);
    check("關閉後選單鍵仍然有效", K->popUiKey() == UI_KEY_OK);
    K->setEnabled(true);
    dn(HID_V);
    check("重新開啟後可以發聲", gSynth.sounding.size() == 1);
  }

  // -------------------------------------------------------------------------
  printf("\n12) 同時按鍵數的硬上限\n");
  {
    reset();
    const uint8_t row[12] = { HID_Z, HID_S, HID_X, HID_D, HID_C, HID_V,
                              HID_G, HID_B, HID_H, HID_N, HID_J, HID_M };
    for (int i = 0; i < 12; i++) dn(row[i]);
    check("最多追蹤 TC_KBD_MAX_DOWN 個，不會寫爆陣列",
          K->downCount() == TC_KBD_MAX_DOWN);
    for (int i = 0; i < 12; i++) up(row[i]);
    check("全部放開後乾淨歸零", gSynth.sounding.empty() && K->downCount() == 0);
  }

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

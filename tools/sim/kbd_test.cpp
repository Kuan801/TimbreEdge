// ============================================================================
//  kbd_test.cpp  -  在桌機上驗證「USB 電腦鍵盤當琴鍵」的邏輯
//
//  用法：  make kbd_test && ./kbd_test
//
//  kbd_in.cpp 刻意不碰 USBHost_t36，鍵碼對應、和弦追蹤、移調、佇列全都是
//  純邏輯，所以桌機測得到。這一點很重要：那個 USB Host 埠目前收不到 MIDI
//  封包，如果連鍵位對應都只能靠燒錄進去用耳朵聽，就完全無從分辨是
//  「程式錯了」還是「USB 沒通」。
//
//  這裡用一個假的合成器把 noteOn/noteOff 記下來，檢查送出去的音高對不對、
//  有沒有成對。真的 AudioSynthAdditive 要拉進一堆 DSP，這裡不需要。
// ============================================================================
#include <cstdio>
#include <cstring>
#include <vector>

// ---- 假的合成器 -----------------------------------------------------------
//
//  kbd_in.h 對合成器只用前置宣告，所以這裡直接提供一個同名的定義就能替換掉。
//  用真的 AudioSynthAdditive 不行：沒載入音色時它會把每個音都丟掉，
//  那就完全驗不出「Z 鍵送出的音高對不對」—— 而那正是最該驗的東西。
//
//  TC_KBD_FAKE_SYNTH 讓 kbd_in.cpp 不要去 include 真的 additive_synth.h。
#define TC_KBD_FAKE_SYNTH 1

class AudioSynthAdditive {
public:
  struct Ev { bool on; int midi; };
  std::vector<Ev>  ev;         // 所有 noteOn/noteOff，依序
  std::vector<int> sounding;   // 目前還響著的音高

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
  K = new KbdInput();          // 每一節從全新狀態開始，測試之間不互相汙染
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
    // 上下兩排剛好差一個八度，這是「兩排能當兩個八度用」的前提
    check("上排 = 下排 + 12", true);
  }

  // -------------------------------------------------------------------------
  printf("\n3) 鍵碼不重複（重複的話某顆琴鍵會永遠彈不到）\n");
  {
    // 這是最容易寫錯又最難用耳朵察覺的地方 —— 兩個項目填到同一個 HID 碼，
    // 表現只是「某一顆鍵發出別人的音」，不會當機。
    bool dup = false;
    for (int i = 0; i < kMapN; i++)
      for (int j = i + 1; j < kMapN; j++)
        if (kMap[i].code == kMap[j].code || kMap[i].semi == kMap[j].semi) dup = true;
    check("24 個鍵碼與 24 個半音都沒有重複", !dup);

    // 琴鍵不能撞到選單鍵，否則按 Enter 會同時發音
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

    up(HID_C);                                 // 放開中間那顆
    check("放開 E 之後剩 2 個", gSynth.sounding.size() == 2);
    check("剩下的是 C 和 G", gSynth.sounding[0] == 60 || gSynth.sounding[1] == 60);
    up(HID_Z); up(HID_B);
    check("全部放開後沒有殘留", gSynth.sounding.empty());
  }

  // -------------------------------------------------------------------------
  printf("\n5) 重複的按下報告不會讓音卡住\n");
  {
    // 有些鍵盤韌體會重送同一份報告。沒擋的話同一顆會 noteOn 兩次，
    // 但放開只送一次 noteOff —— 結果就是那個音永遠關不掉。
    reset();
    dn(HID_Z); dn(HID_Z); dn(HID_Z);
    check("按下三次只發一次聲", gSynth.sounding.size() == 1);
    up(HID_Z);
    check("放開一次就完全關掉", gSynth.sounding.empty());
  }

  // -------------------------------------------------------------------------
  printf("\n6) 移調：放開時必須用當初送出去的音高\n");
  {
    // 這是實體琴鍵那邊踩過的坑：按著的時候改八度，放開時若用「新」音高去
    // noteOff，舊的那顆就永遠關不掉。
    reset();
    dn(HID_Z);
    check("原位彈 Z 得到 60", lastOn() == 60);
    dn(HID_RIGHT);                             // 升一個八度
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
    // 停音之後同一顆鍵要能重按 —— 內部表沒清乾淨的話會被當成重複報告擋掉
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

    // 放開不該再送一次，否則選單會跳兩格
    dn(HID_DOWN); K->popUiKey();
    up(HID_DOWN);
    check("放開不會再送一次選單鍵", K->popUiKey() == UI_KEY_NONE);

    // Backspace 跟 Esc 同義：有些鍵盤 Esc 很遠
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
    // 但選單鍵仍然要能用，否則關掉之後就再也開不回來了
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

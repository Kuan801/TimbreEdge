// ============================================================================
//  wavhdr_test.cpp  -  is the WAV header still valid after "power lost mid-record"
//
//  Usage:  make wavhdr_test && ./wavhdr_test
//
//  Why this test exists
//  --------------------
//  The RIFF/data lengths in a WAV used to be patched only in close(). Lose power,
//  press reset, or pull the SD card before a recording or performance finishes and
//  several MB of audio are on the card, but the data length in the header is still
//  whatever open() wrote (0 for StereoCapture).
//
//  The hard part about such a file is that some players read it fine:
//      Windows Explorer / Media Player -> declares it corrupt, refuses to play
//      ffmpeg / QuickTime / Audacity   -> scan to end of file and play it anyway
//  So the symptom becomes "it plays on the Mac but Windows says the file is
//  broken", which is easily mistaken for a player problem rather than a writer one.
//
//  Hence WavWriter::flushHeader(), which recorder.cpp calls every 2 s. This test
//  simulates "power loss" as "never call close()" and inspects the bytes on disk.
// ============================================================================
#include "../../wav_io.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static int gFail = 0;
static void check(const char *what, bool ok, const char *detail = "") {
  printf("  %-48s %s %s\n", what, ok ? "通過" : "**失敗**", detail);
  if (!ok) gFail++;
}

// Read the bytes on disk directly, not through WavWriter -- the file itself is
// what is under test
struct Hdr { bool riff; uint32_t riffSize, dataSize; uint32_t fileSize; };

static Hdr readHdr(const char *path) {
  Hdr h = {false, 0, 0, 0};
  std::string full = std::string("./") + path;
  FILE *f = fopen(full.c_str(), "rb");
  if (!f) return h;
  fseek(f, 0, SEEK_END);
  h.fileSize = (uint32_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t b[48];
  if (fread(b, 1, 48, f) == 48) {
    h.riff = (memcmp(b, "RIFF", 4) == 0) && (memcmp(b + 8, "WAVE", 4) == 0);
    memcpy(&h.riffSize, b + 4,  4);
    memcpy(&h.dataSize, b + 40, 4);
  }
  fclose(f);
  return h;
}

// How a player decides whether this file is playable:
//   the RIFF/WAVE markers are present, the data length is not 0, and it does not
//   claim more data than actually exists.
//
// Deliberately not requiring "RIFF length exactly equals file size - 8": the few
// seconds written after the last patch are not accounted for in the header yet, so
// there will be undeclared bytes at the end of the file. Players ignore that tail
// and play anyway -- declaring *less* than exists is safe, declaring *more* is what
// breaks.
static bool playable(const Hdr &h) {
  return h.riff && h.dataSize > 0 && h.dataSize + 44 <= h.fileSize;
}

static int16_t gBuf[512];

int main() {
  for (int i = 0; i < 512; i++) gBuf[i] = (int16_t)(i * 37);

  printf("\n=== 正常收尾（有 close）===\n");
  {
    WavWriter w;
    check("開得起來", w.open("_hdrtest_ok.wav", 44100, 2));
    for (int i = 0; i < 40; i++) w.writeSamples(gBuf, 512);
    w.close();
    Hdr h = readHdr("_hdrtest_ok.wav");
    char m[96];
    snprintf(m, sizeof(m), "(檔案 %u，data %u)", h.fileSize, h.dataSize);
    check("是合法 WAV 而且播得出來", playable(h), m);
    check("data 長度等於實際寫進去的量", h.dataSize == 40u * 512u * 2u);
    check("RIFF 長度 = 檔案大小 - 8", h.riffSize == h.fileSize - 8);
  }

  printf("\n=== 錄到一半斷電（沒有 close）===\n");
  {
    WavWriter w;
    w.open("_hdrtest_cut.wav", 44100, 2);
    for (int i = 0; i < 20; i++) w.writeSamples(gBuf, 512);
    w.flushHeader();                       // This is what recorder.cpp does every 2 s
    for (int i = 0; i < 5; i++) w.writeSamples(gBuf, 512);
    // "Power loss" here: never call close(), just leave the file on the card
    Hdr h = readHdr("_hdrtest_cut.wav");
    char m[96];
    snprintf(m, sizeof(m), "(檔案 %u，data %u)", h.fileSize, h.dataSize);
    check("仍然是播得出來的 WAV", playable(h), m);
    check("data 長度是上一次補正時的量（最多損失那 2 秒）",
          h.dataSize == 20u * 512u * 2u);
    // How much of the data written after the patch actually reached the card
    // depends on flushing -- power loss eats whatever is still cached by
    // definition. All this checks is that the already-patched portion is
    // definitely there, which is what the mechanism guarantees.
    check("補正涵蓋的資料確實已經在檔案裡",
          h.fileSize >= 44u + 20u * 512u * 2u);
  }

  printf("\n=== 負對照：如果沒有 flushHeader ===\n");
  {
    // This section deliberately reproduces the old behaviour, to prove the check
    // above actually catches something. Without the negative control, "it plays"
    // might only mean the check is too lenient.
    WavWriter w;
    w.open("_hdrtest_old.wav", 44100, 2);
    for (int i = 0; i < 20; i++) w.writeSamples(gBuf, 512);
    // No flushHeader and no close -- this is what power loss produced before the fix
    Hdr h = readHdr("_hdrtest_old.wav");
    char m[96];
    snprintf(m, sizeof(m), "(檔案 %u，data 卻是 %u)", h.fileSize, h.dataSize);
    check("舊行為確實會留下 data=0 的壞檔", !playable(h) && h.dataSize == 0, m);
  }

  printf("\n=== 預先寫入長度的路徑（Recorder 用）===\n");
  {
    // Recorder::start writes the expected length into the header up front. If the
    // recording runs to completion that is exact; if it stops early the header
    // claims more than exists -- also a broken file, and close/flushHeader has to
    // fix it.
    WavWriter w;
    w.open("_hdrtest_short.wav", 44100, 1, 44100 * 3);   // Declare 3 s
    for (int i = 0; i < 10; i++) w.writeSamples(gBuf, 512);   // Write only 0.12 s
    Hdr before = readHdr("_hdrtest_short.wav");
    check("補正前：標頭宣告的比實際資料多（壞檔）",
          before.dataSize > before.fileSize - 44);
    w.flushHeader();
    Hdr after = readHdr("_hdrtest_short.wav");
    check("補正後：變成播得出來的合法 WAV", playable(after));
    w.close();
  }

  remove("./_hdrtest_ok.wav");
  remove("./_hdrtest_cut.wav");
  remove("./_hdrtest_old.wav");
  remove("./_hdrtest_short.wav");

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

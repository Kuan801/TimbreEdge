// ============================================================================
//  tools/sim/fake_mtp/MTP_Teensy.h
//
//  KurtE 的 MTP_Teensy 在桌機上的替身，只為了讓 `make inocheck-mtp` 能把
//  TimbreClone.ino 裡 TC_HAS_MTP=1 那條路徑也編譯一次。
//
//  為什麼需要它：那些 MTP 程式碼包在 #if TC_HAS_MTP 裡，而桌機這邊
//  USB_MTPDISK_SERIAL 永遠沒有定義，所以平常的 inocheck 根本走不到 ——
//  等於那段程式在真的燒錄之前完全沒有編譯涵蓋。fake_usbhost 當初就是為了
//  同一個理由存在的，這裡照抄那個做法。
//
//  ── 這個檔案的教訓 ────────────────────────────────────────────────────
//  第一版把 send_DeviceResetEvent() 寫成公開的 void，inocheck-mtp 順利通過，
//  然後在真的機器上編譯失敗：「'int MTP_class::send_DeviceResetEvent()'
//  is private within this context」。
//
//  真相是 MTP_Teensy 把所有 send_*Event() 都包在 #if USE_EVENTS == 1 裡，
//  而那個巨集在函式庫的編譯單元裡是關的；名稱查找因此落到私有區那份同名宣告。
//  替身寫得比真品寬鬆，等於把這個檢查變成一張假的通行證 —— 比沒有檢查更糟，
//  因為它會讓人以為已經驗過了。
//
//  所以下面刻意複製真品的結構：USE_EVENTS 預設 0、事件函式包在條件裡、
//  私有區留一份同名宣告。這樣桌機這邊會複製出一模一樣的編譯錯誤。
//  替身的規則是「照抄，不要放寬」。
//
//  介面出處：https://github.com/KurtE/MTP_Teensy 的 src/MTP_Teensy.h
//  這個檔案永遠不會被燒進 Teensy：韌體端的 include 路徑不含 tools/sim。
// ============================================================================
#pragma once

#include <stdint.h>
#include <SD.h>

#ifndef USE_EVENTS
#define USE_EVENTS 0        // 跟真品一樣預設關閉
#endif

class MTP_class {
 private:
  // 真品在私有區也有這一份。公開那份被 USE_EVENTS 關掉時，名稱查找會找到它，
  // 於是錯誤訊息是「is private」而不是「沒有這個成員」。
  int send_DeviceResetEvent(void) { return 0; }

 public:
  int      begin() { return 1; }
  void     loop(void) { }
  uint32_t addFilesystem(SDClass &disk, const char *diskname) {
    (void)disk; (void)diskname; return 0;
  }
  uint32_t getFilesystemCount(void) { return 1; }

#if USE_EVENTS == 1
  int  send_Event(uint16_t eventCode) { (void)eventCode; return 0; }
  int  send_addObjectEvent(uint32_t p1) { (void)p1; return 0; }
  int  send_removeObjectEvent(uint32_t p1) { (void)p1; return 0; }
  int  send_StorageInfoChangedEvent(uint32_t p1) { (void)p1; return 0; }
  int  send_StoreAddedEvent(uint32_t store) { (void)store; return 0; }
  int  send_StoreRemovedEvent(uint32_t store) { (void)store; return 0; }
  bool send_addObjectEvent(uint32_t store, const char *pathname) {
    (void)store; (void)pathname; return true;
  }
  bool send_removeObjectEvent(uint32_t store, const char *pathname) {
    (void)store; (void)pathname; return true;
  }
#endif
};

extern MTP_class MTP;

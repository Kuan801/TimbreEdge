// ============================================================================
//  tools/sim/fake_mtp/MTP_Teensy.h
//
//  A desktop stand-in for KurtE's MTP_Teensy, only so that `make inocheck-mtp`
//  also compiles the TC_HAS_MTP=1 path through TimbreClone.ino once.
//
//  Why it is needed: that MTP code sits inside #if TC_HAS_MTP, and on the
//  desktop USB_MTPDISK_SERIAL is never defined, so the ordinary inocheck never
//  reaches it -- meaning that code had no compile coverage at all until an
//  actual flash. fake_usbhost exists for exactly the same reason; this copies
//  that approach.
//
//  ── What this file taught us ──────────────────────────────────────────
//  The first version declared send_DeviceResetEvent() as a public void,
//  inocheck-mtp passed happily, and then the build failed on the real machine:
//  "'int MTP_class::send_DeviceResetEvent()' is private within this context".
//
//  The truth is that MTP_Teensy wraps every send_*Event() in #if USE_EVENTS == 1,
//  and that macro is off inside the library's own translation unit; name lookup
//  therefore lands on the identically named declaration in the private section.
//  A stand-in written looser than the real thing turns this check into a fake
//  pass -- worse than no check at all, because it makes you believe you already
//  verified it.
//
//  So the structure of the real thing is reproduced deliberately below:
//  USE_EVENTS defaults to 0, the event functions are inside the conditional,
//  and the private section keeps an identically named declaration. That way the
//  desktop reproduces exactly the same compile error.
//  The rule for a stand-in is "copy it, do not relax it".
//
//  Interface source: src/MTP_Teensy.h in https://github.com/KurtE/MTP_Teensy
//  This file never gets flashed to the Teensy: the firmware include path does
//  not contain tools/sim.
// ============================================================================
#pragma once

#include <stdint.h>
#include <SD.h>

#ifndef USE_EVENTS
#define USE_EVENTS 0        // Off by default, same as the real thing
#endif

class MTP_class {
 private:
  // The real thing has this copy in the private section too. When the public one
  // is switched off by USE_EVENTS, name lookup finds this one, so the error reads
  // "is private" instead of "no such member".
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

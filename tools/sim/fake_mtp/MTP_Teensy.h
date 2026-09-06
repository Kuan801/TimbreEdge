#pragma once

#include <stdint.h>
#include <SD.h>

#ifndef USE_EVENTS
#define USE_EVENTS 0
#endif

class MTP_class {
 private:

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

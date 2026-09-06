CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++14 -DTC_USE_OLED=0 -I. -Wall -Wno-unused-variable -Wno-format-security

COMMON = sim_support.cpp ../../analyzer.cpp ../../profile.cpp ../../timbre_model.cpp ../../trainer.cpp \
      ../../additive_synth.cpp ../../score.cpp ../../player.cpp ../../wav_io.cpp \
      ../../ui.cpp ../../kbd_in.cpp ../../rec_check.cpp ../../trigger.cpp \
      ../../display.cpp

SRC = sim_main.cpp $(COMMON)

HDR = $(wildcard ../../*.h) $(wildcard *.h)

all: sim xrange ui_test score_test kbd_test display_test bank_test wavname_test wavhdr_test reccheck_test trigger_test setdir_test evict_test usbcheck inocheck chaincheck

sim: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o sim $(SRC) -lm

xrange: xrange.cpp $(COMMON) $(HDR)
	$(CXX) $(CXXFLAGS) -o xrange xrange.cpp $(COMMON) -lm

score_test: score_test.cpp ../../score.cpp ../../score.h
	$(CXX) $(CXXFLAGS) -o score_test score_test.cpp ../../score.cpp

ui_test: ui_test.cpp ../../ui.cpp ../../ui.h
	$(CXX) $(CXXFLAGS) -o ui_test ui_test.cpp ../../ui.cpp

kbd_test: kbd_test.cpp ../../kbd_in.cpp ../../kbd_in.h ../../ui.cpp ../../ui.h
	$(CXX) $(CXXFLAGS) -o kbd_test kbd_test.cpp ../../ui.cpp

display_test: display_test.cpp ../../display.cpp ../../display.h
	$(CXX) $(CXXFLAGS) -o display_test display_test.cpp ../../display.cpp

bank_test: bank_test.cpp ../../profile.cpp ../../profile.h sim_support.cpp
	$(CXX) $(CXXFLAGS) -o bank_test bank_test.cpp ../../profile.cpp sim_support.cpp ../../wav_io.cpp ../../display.cpp -lm

wavname_test: wavname_test.cpp ../../wav_io.cpp ../../wav_io.h sim_support.cpp
	$(CXX) $(CXXFLAGS) -o wavname_test wavname_test.cpp ../../wav_io.cpp sim_support.cpp ../../display.cpp -lm

wavhdr_test: wavhdr_test.cpp ../../wav_io.cpp ../../wav_io.h sim_support.cpp
	$(CXX) $(CXXFLAGS) -o wavhdr_test wavhdr_test.cpp ../../wav_io.cpp sim_support.cpp ../../display.cpp -lm

chaincheck: chaincheck.cpp $(COMMON) $(HDR)
	$(CXX) $(CXXFLAGS) -o chaincheck chaincheck.cpp $(COMMON) -lm

evict_test: evict_test.cpp ../../profile.cpp sim_support.cpp ../../wav_io.cpp ../../display.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -o evict_test evict_test.cpp ../../profile.cpp sim_support.cpp ../../wav_io.cpp ../../display.cpp -lm

setdir_test: setdir_test.cpp ../../wav_io.cpp sim_support.cpp ../../display.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -o setdir_test setdir_test.cpp ../../wav_io.cpp sim_support.cpp ../../display.cpp -lm

trigger_test: trigger_test.cpp ../../trigger.cpp ../../trigger.h ../../config.h
	$(CXX) $(CXXFLAGS) -o trigger_test trigger_test.cpp ../../trigger.cpp -lm

reccheck_test: reccheck_test.cpp ../../rec_check.cpp ../../rec_check.h
	$(CXX) $(CXXFLAGS) -o reccheck_test reccheck_test.cpp ../../rec_check.cpp -lm

recscan: recscan.cpp $(COMMON) $(HDR)
	$(CXX) $(CXXFLAGS) -o recscan recscan.cpp $(COMMON) -lm

envdist: envdist.cpp $(COMMON) $(HDR)
	$(CXX) $(CXXFLAGS) -o envdist envdist.cpp $(COMMON) -lm

usbcheck: ../../kbd_in.cpp ../../kbd_in.h ../../config.h fake_usbhost/USBHost_t36.h
	$(CXX) $(CXXFLAGS) -DTC_USE_USB_KBD=1 -Ifake_usbhost -fsyntax-only ../../kbd_in.cpp
	@echo "usbcheck: kbd_in.cpp 的 USB Host 路徑編譯通過"
	@if [ "$$($(CXX) $(CXXFLAGS) -D__IMXRT1062__ -Ifake_usbhost -E ../../kbd_in.cpp 2>/dev/null | grep -c 'usbHost.begin')" -eq 0 ]; then \
	    echo "usbcheck: 失敗 —— Teensy 路徑上 USB Host 區段被編掉了（config.h 沒進來？）"; \
	    exit 1; \
	fi
	@echo "usbcheck: Teensy 路徑上 USB Host 區段確實有被編進去"

inocheck: ../../TimbreClone.ino $(HDR) Audio.h Wire.h fake_usbhost/USBHost_t36.h
	@cp ../../TimbreClone.ino ./_inocheck.cpp
	@$(CXX) $(CXXFLAGS) -DTC_INO_CHECK=1 -DTC_USE_USB_KBD=1 \
	        -Ifake_usbhost -I../.. -fsyntax-only ./_inocheck.cpp
	@rm -f ./_inocheck.cpp
	@echo "inocheck: TimbreClone.ino 語法檢查通過"

inocheck-mtp: ../../TimbreClone.ino $(HDR) Audio.h Wire.h fake_usbhost/USBHost_t36.h fake_mtp/MTP_Teensy.h
	@cp ../../TimbreClone.ino ./_inocheck_mtp.cpp
	@$(CXX) $(CXXFLAGS) -DTC_INO_CHECK=1 -DTC_USE_USB_KBD=1 -DUSB_MTPDISK_SERIAL=1 \
	        -Ifake_usbhost -Ifake_mtp -I../.. -fsyntax-only ./_inocheck_mtp.cpp
	@rm -f ./_inocheck_mtp.cpp
	@echo "inocheck-mtp: TimbreClone.ino（USB Type = Serial + MTP Disk）語法檢查通過"

clean:
	rm -f sim recscan xrange ui_test score_test kbd_test display_test bank_test wavname_test wavhdr_test reccheck_test trigger_test setdir_test evict_test envdist chaincheck _inocheck.cpp _inocheck_mtp.cpp

.PHONY: clean all

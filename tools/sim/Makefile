# 桌機模擬器：用同一份 DSP 程式碼把半音階算成 WAV
# 用法：make && ./sim out.wav MODEL.BIN note1.wav [note2.wav ...]

CXX      ?= g++
# 桌機沒有 OLED，關掉它（display.cpp 會編成空函式）
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

# 用一小段音域訓練、往外推到整個音域，量移調距離對音色的影響
xrange: xrange.cpp $(COMMON) $(HDR)
	$(CXX) $(CXXFLAGS) -o xrange xrange.cpp $(COMMON) -lm

# 選單導航是純狀態機，不用 Arduino 也不用 sim_support
# 樂譜是純資料產生器，桌機直接驗證：音符數、時間軸、音域、同時發聲數
score_test: score_test.cpp ../../score.cpp ../../score.h
	$(CXX) $(CXXFLAGS) -o score_test score_test.cpp ../../score.cpp

ui_test: ui_test.cpp ../../ui.cpp ../../ui.h
	$(CXX) $(CXXFLAGS) -o ui_test ui_test.cpp ../../ui.cpp

# USB 電腦鍵盤的鍵位對應也是純邏輯。它直接 #include kbd_in.cpp 並塞一個假的
# 合成器進去，所以連「Z 鍵送出的音高對不對」都驗得到 —— 這在硬體上只能用
# 耳朵聽，而目前那個 USB Host 埠本來就有問題，更不能靠它來判斷程式對不對。
kbd_test: kbd_test.cpp ../../kbd_in.cpp ../../kbd_in.h ../../ui.cpp ../../ui.h
	$(CXX) $(CXXFLAGS) -o kbd_test kbd_test.cpp ../../ui.cpp

# 「選單不能蓋掉狀態面板」。這條規則壞掉時螢幕還在更新，看起來不像當機，
# 但採樣面板一瞬間就被蓋回去 —— 使用者只會說「按了 Auto sampling 沒反應」。
# display.cpp 的狀態邏輯在 #if TC_USE_OLED 之外，所以桌機驗得到。
display_test: display_test.cpp ../../display.cpp ../../display.h
	$(CXX) $(CXXFLAGS) -o display_test display_test.cpp ../../display.cpp

# 音色庫的入庫規則與「換樂器沒清空」警告。
# 門檻值是用真實素材量出來的（見 config.h），這裡驗機制。
# 用合成 profile 不吃素材檔，換一台電腦也跑得動。
bank_test: bank_test.cpp ../../profile.cpp ../../profile.h sim_support.cpp
	$(CXX) $(CXXFLAGS) -o bank_test bank_test.cpp ../../profile.cpp sim_support.cpp ../../wav_io.cpp ../../display.cpp -lm

# 「哪些檔案可以刪」。這是最不能出錯的一段 —— 判斷錯了就刪掉使用者的素材，
# 而且不可逆。規則刻意訂得嚴，寧可漏刪也不要誤刪。
wavname_test: wavname_test.cpp ../../wav_io.cpp ../../wav_io.h sim_support.cpp
	$(CXX) $(CXXFLAGS) -o wavname_test wavname_test.cpp ../../wav_io.cpp sim_support.cpp ../../display.cpp -lm

# WAV 標頭在「錄到一半斷電」之後還合不合法。
# 這條規則壞掉的症狀非常難查：檔案在 Mac / ffmpeg 上照樣播得出來，
# 只有 Windows 說損毀 —— 很容易被當成播放器的問題。
wavhdr_test: wavhdr_test.cpp ../../wav_io.cpp ../../wav_io.h sim_support.cpp
	$(CXX) $(CXXFLAGS) -o wavhdr_test wavhdr_test.cpp ../../wav_io.cpp sim_support.cpp ../../display.cpp -lm

# 量錄音鏈的頻率響應。給「參考檔 + 用你的麥克風錄的同一個音」，
# 印出各頻段的相對增益。用途是把「聽起來怪怪的」變成可以追蹤的數字，
# 調整麥克風位置時可以一邊改一邊量。
chaincheck: chaincheck.cpp $(COMMON) $(HDR)
	$(CXX) $(CXXFLAGS) -o chaincheck chaincheck.cpp $(COMMON) -lm

# 量測工具（不是測試）：印出素材兩兩的音色距離分布，用來決定警告門檻。
# 素材換了就重跑，把 config.h 裡那組數字更新掉。
# 音色庫滿了要犧牲誰：無聲失效，換錯人只會讓某個音區沒素材可用
evict_test: evict_test.cpp ../../profile.cpp sim_support.cpp ../../wav_io.cpp ../../display.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -o evict_test evict_test.cpp ../../profile.cpp sim_support.cpp ../../wav_io.cpp ../../display.cpp -lm

# 採樣資料夾是不可逆的操作：編號算錯會蓋掉上一輪，刪錯路徑會刪掉使用者的素材。
# 用桌機的 SD 模擬層在暫存目錄跑一遍，含「不該刪的有沒有被刪」的負對照。
setdir_test: setdir_test.cpp ../../wav_io.cpp sim_support.cpp ../../display.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -o setdir_test setdir_test.cpp ../../wav_io.cpp sim_support.cpp ../../display.cpp -lm

# 觸發判斷是純狀態機。真機上「沒人演奏卻一直錄」要坐著等好幾分鐘才看得到，
# 桌機餵合成訊號兩秒跑完，而且分得出來是門檻還是重新武裝的問題。
trigger_test: trigger_test.cpp ../../trigger.cpp ../../trigger.h ../../config.h
	$(CXX) $(CXXFLAGS) -o trigger_test trigger_test.cpp ../../trigger.cpp -lm

# 錄音判定是純數值規則，一改門檻就要能立刻驗證正例與負例
reccheck_test: reccheck_test.cpp ../../rec_check.cpp ../../rec_check.h
	$(CXX) $(CXXFLAGS) -o reccheck_test reccheck_test.cpp ../../rec_check.cpp -lm

# 拿真實素材跑一遍判定（分析器 + 規則接起來的結果）
recscan: recscan.cpp $(COMMON) $(HDR)
	$(CXX) $(CXXFLAGS) -o recscan recscan.cpp $(COMMON) -lm

envdist: envdist.cpp $(COMMON) $(HDR)
	$(CXX) $(CXXFLAGS) -o envdist envdist.cpp $(COMMON) -lm

# 用假的 USBHost_t36 把 kbd_in.cpp 的 USB Host 路徑編一次。
# 平常那段被 TC_USE_USB_KBD=0 跳過，桌機編得過不代表 Teensy 編得過 ——
# 這個目標就是為了在燒錄之前擋下那類錯誤（例如撞到 Print.h 的 #define DEC）。
usbcheck: ../../kbd_in.cpp ../../kbd_in.h ../../config.h fake_usbhost/USBHost_t36.h
	$(CXX) $(CXXFLAGS) -DTC_USE_USB_KBD=1 -Ifake_usbhost -fsyntax-only ../../kbd_in.cpp
	@echo "usbcheck: kbd_in.cpp 的 USB Host 路徑編譯通過"
	@# --- 上面那一行自己補了 -DTC_USE_USB_KBD=1，所以它「永遠」在測活的那條路。
	@# 測試把自己要驗的前提補上了，於是漏掉了真正發生過的那個 bug：
	@# kbd_in.cpp 忘了 include config.h，巨集沒定義，#if 當成 0，
	@# 整段 USB Host 被靜靜編掉換成兩個空函式 —— 編得過、燒得進、插鍵盤沒反應。
	@#
	@# 所以要另外驗一次「不給任何 -D，只靠 config.h 自己決定」的情況：
	@# 假裝在 Teensy 上（__IMXRT1062__），展開之後 usbHost.begin 必須還在。
	@if [ "$$($(CXX) $(CXXFLAGS) -D__IMXRT1062__ -Ifake_usbhost -E ../../kbd_in.cpp 2>/dev/null | grep -c 'usbHost.begin')" -eq 0 ]; then \
	    echo "usbcheck: 失敗 —— Teensy 路徑上 USB Host 區段被編掉了（config.h 沒進來？）"; \
	    exit 1; \
	fi
	@echo "usbcheck: Teensy 路徑上 USB Host 區段確實有被編進去"

# 把 TimbreClone.ino 在桌機上編一次。
#
# .ino 一直是唯一沒有編譯涵蓋的檔案，已經漏掉兩次錯：選單頁數宣告得比實際項目
# 少一項，以及 gUiOctave 宣告在使用點後面。後者特別容易犯 —— Arduino 的預處理
# 只會自動補「函式原型」，不補變數宣告，所以在 .ino 裡變數的位置是有意義的，
# 但一般 C++ 直覺不會想到這件事。
#
# 副檔名要換成 .cpp，g++ 才認得。
#
# 一個誠實的差異：g++ 比 Arduino 嚴格 —— Arduino 會自動補函式原型，g++ 不會。
# 所以「函式用在定義之前」這裡會報錯、Arduino 卻能過。真的遇到就在 .ino 裡
# 自己補一行前置宣告（檔案裡的 uiRenderFwd 就是這樣處理的），不要改這個目標。
inocheck: ../../TimbreClone.ino $(HDR) Audio.h Wire.h fake_usbhost/USBHost_t36.h
	@cp ../../TimbreClone.ino ./_inocheck.cpp
	@$(CXX) $(CXXFLAGS) -DTC_INO_CHECK=1 -DTC_USE_USB_KBD=1 \
	        -Ifake_usbhost -I../.. -fsyntax-only ./_inocheck.cpp
	@rm -f ./_inocheck.cpp
	@echo "inocheck: TimbreClone.ino 語法檢查通過"

# 同一份 .ino，但把 USB Type 假裝成「Serial + MTP Disk」，好讓 #if TC_HAS_MTP
# 底下那些程式碼也被編一次。
#
# 沒有這個目標的話，MTP 那段的編譯涵蓋率是零 —— 平常的 inocheck 走不到它，
# 唯一會走到的地方是使用者按下上傳之後的那 90 秒。fake_usbhost 當初就是為了
# 同樣的理由存在，MTP_Teensy.h 的替身放在 fake_mtp/。
inocheck-mtp: ../../TimbreClone.ino $(HDR) Audio.h Wire.h fake_usbhost/USBHost_t36.h fake_mtp/MTP_Teensy.h
	@cp ../../TimbreClone.ino ./_inocheck_mtp.cpp
	@$(CXX) $(CXXFLAGS) -DTC_INO_CHECK=1 -DTC_USE_USB_KBD=1 -DUSB_MTPDISK_SERIAL=1 \
	        -Ifake_usbhost -Ifake_mtp -I../.. -fsyntax-only ./_inocheck_mtp.cpp
	@rm -f ./_inocheck_mtp.cpp
	@echo "inocheck-mtp: TimbreClone.ino（USB Type = Serial + MTP Disk）語法檢查通過"

clean:
	rm -f sim recscan xrange ui_test score_test kbd_test display_test bank_test wavname_test wavhdr_test reccheck_test trigger_test setdir_test evict_test envdist chaincheck _inocheck.cpp _inocheck_mtp.cpp

.PHONY: clean all

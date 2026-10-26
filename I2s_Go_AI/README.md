# I2S-GO v1.1 專案集（驗證成功的程式）

陳志弘老師設計的 **ESP32 I2S-GO v1.1** 擴展板上，實測成功的程式備份。
核心板：WEMOS LOLIN32 Lite（ESP32-D0WDQ6，4MB flash，**無 PSRAM**）。

---

## 📁 內容

| 資料夾 | 程式 | 語言 | 狀態 |
|---|---|---|---|
| `esp32_beep10_micropython/` | `beep10.py` — 喇叭發出 10 聲嗶 | MicroPython | ✅ 成功 |
| `esp32_marquee_arduino/` | `marquee.ino` — TFT 顯示 "Claude control" 跑馬燈 | Arduino | ✅ 成功 |
| `esp32_webradio_arduino/` | `webradio.ino` — 網路電台機（SomaFM 串流） | Arduino | ✅ 成功 |
| `esp32_voice_assistant/` | 語音對話助理（Mac 當大腦，見子資料夾 README） | Arduino + Python | ✅ 成功 |
| `rpi0_led_micropython/` | `led_control.py` — Raspberry Pi Zero 板載 LED 控制 | Python | ✅ 成功（此為 rpi0，非 ESP32） |

---

## ⚠️ 關鍵經驗（踩過的坑，務必記住）

1. **燒錄速度一定用 460800，不要用預設 921600。**
   這條 USB 線/板子在 921600 下訊號失真，會**靜默刷錄失敗**（esptool 報
   `Invalid head of packet ... serial noise or corruption`，但 arduino-cli 可能回 exit 0 假成功）。
   - esptool：`--baud 460800`
   - arduino-cli：FQBN 加 `UploadSpeed=460800`

2. **原廠範例 / ESP8266Audio 要用 ESP32 core 1.0.6 編譯。**
   新版 core 2.0.17 與 2021 年的 ESP8266Audio 不相容（`'SPECIAL' 未宣告`、
   `i2s_write_bytes` 已移除、編譯器 ICE）。webradio 也是用 1.0.6 編的。

3. **TFT 顯示用 Arduino TFT_eSPI（Setup100_ESP32_I2S_GO.h），不要用 MicroPython 的 russhughes 驅動**
   （那個驅動在這片 ST7789 面板上點不亮）。

4. **硬體接觸**：排針/模塊（TFT、MAX98357A、喇叭）搬動後容易鬆，沒反應時先重插再懷疑程式。

5. **音訊別開太大聲**：USB 5V 供電時大音量會瞬間掉電重啟（原廠警告）。用電池/穩定 5V 較好。

---

## 🔌 I2S-GO 官方腳位（出處：原廠 PDF 說明 p.6）

```
INMP441 麥克風 : WS=33, SCK=25, SD=26 (3.3V)
MAX98357A 功放 : LRC=12, BCLK=13, DIN=15
ST7789 TFT     : SCLK=18, MOSI=19, CS=5, DC=16, RES=23, BLK=4 (背光 HIGH 亮)
microSD (SPI)  : MISO=2, CLK=17, MOSI=22, CS=5
RGBW LED       : data=32
按鈕           : 左=IO0, 右=IO35
I2C            : SDA=14, SCL=27
```

---

## 🛠️ 怎麼用

### MicroPython（beep10）
板子需先燒 MicroPython 韌體（esptool，460800），再用 mpremote 跑：
```bash
esptool.py --port /dev/cu.usbserial-2120 --baud 460800 write_flash -z 0x1000 <micropython.bin>
mpremote connect /dev/cu.usbserial-2120 run esp32_beep10_micropython/beep10.py
```

### Arduino（marquee / webradio）
用 arduino-cli（core 1.0.6），把 sketch 資料夾編譯上傳：
```bash
arduino-cli compile --upload -p /dev/cu.usbserial-2120 \
  --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=460800" \
  esp32_webradio_arduino/webradio
```
需要的函式庫：TFT_eSPI（含 Setup100）、ESP8266Audio（webradio 用）。

> **webradio.ino 內含 WiFi 帳密（明碼）**。要改網路或電台，直接編輯檔案開頭的
> `WIFI_SSID` / `WIFI_PASS` 與 `stations[]`。目前還留有除錯用的 serial 心跳訊息（無害）。

### Raspberry Pi（led_control）
這支是給 **rpi0（Raspberry Pi Zero W, 192.168.68.142）** 用的，控制板載 ACT LED：
```bash
sudo python3 led_control.py on | off | blink [-n 次數] [-i 間隔] | restore
```

---

*由 Claude 協助建立於 2026-10（ESP32 I2S-GO 實作過程的成功程式備份）。*

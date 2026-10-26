# I2S-GO 語音對話助理「小智」（Mac 當大腦）

ESP32 I2S-GO 板當終端、Mac 當運算大腦的**本機語音助理**。
**免費 / 免 API 金鑰 / 本機運算 / 隱私不出門**（唯一走網路的是 edge-tts 的微軟免費語音端點，以及網路電台來源）。

按住左鍵說話 → Mac 辨識＋思考＋發音 → 板子喇叭唸出回答、TFT 逐字顯示中文、還能**用語音控制硬體**和**播放網路電台**。

---

## 🏗️ 架構

```
┌────────── ESP32 I2S-GO (終端) ──────────┐        ┌──────────── Mac (大腦) ────────────┐
│ 按住左鍵說話                             │        │ server.py  (Flask, port 5005)        │
│ INMP441 麥克風 ─錄音→ 串流上傳(chunked)   │──LAN──>│   Whisper small    語音辨識          │
│ MAX98357A 喇叭 ←串流播放─ 回傳 WAV        │<───────│   OpenCC s2twp     簡→繁(台灣)       │
│ ST7789 TFT   逐字顯示中文回覆(Mac渲染圖)  │        │   Gemma 2 9B(Ollama) 對話+記憶+即時   │
│ RGBW LED(G32) 語音控制 顏色/序列/閃爍     │        │   edge-tts         中文發音          │
│ 喇叭         嗶聲 / 網路電台音樂          │        │   PIL 渲染文字圖 / ffmpeg 轉音樂串流  │
│ 右鍵(IO35) 停止音樂                       │        │ Ollama serve (127.0.0.1:11434)       │
└──────────────────────────────────────────┘        └──────────────────────────────────────┘
```

---

## 🧩 技術組合

| 環節 | 用的 | 來源 / 備註 |
|---|---|---|
| 語音辨識 STT | **faster-whisper `small`** | OpenAI Whisper，本機 CPU |
| 簡→繁 | **OpenCC `s2twp`** | 統一台灣繁體（辨識＋回覆） |
| 對話 LLM | **Gemma 2 9B**（Ollama） | 🇺🇸 Google，非中國、政治題不審查、多輪記憶 |
| 發音 TTS | **edge-tts `zh-TW-HsiaoChenNeural`** | 微軟神經語音「小智嗓」，免金鑰 |
| TFT 中文 | **Pillow + STHeiti** 渲染成點陣 | ESP32 不需中文字型，只貼圖/逐字 |
| 音樂 | **ffmpeg**（imageio-ffmpeg）轉 44.1kHz 串流 | 網路電台 SomaFM |

---

## ✨ 功能一覽

- 🎤 **Push-to-talk 串流錄音**：按住左鍵邊錄邊傳，最長 10 秒
- 🧠 **多輪對話記憶**：記最近 6 輪，可接續；說「重新開始」清空
- 🕐 **即時資訊**：自動帶入時間/日期；問天氣自動查 open-meteo（免金鑰，預設台北）
- 📝 **全程繁體** + **TFT 逐字顯示**中文回覆（打字機效果，另一核心與語音同步）
- 💡 **語音控制 GPIO**：RGBW LED 變色 / **顏色序列** / 閃爍、喇叭嗶聲
- 🎵 **網路電台音樂**（44.1kHz），播放中右鍵停止
- 😀 **待機表情臉**：兩隻眼睛會到處轉、眨眼、換各種表情
- 🎭 **對話表情**：聆聽臉 → 思考臉(眼往上看) → 說話臉(嘴巴開合)；**依回答內容變心情**（開心/難過/生氣/驚訝/愛心,Mac 判斷經 `X-Mood` 標頭傳回）
- 🔊 **右鍵調講話音量**：待機時按右鍵循環 25→50→75→100%

### 語音指令範例

| 說 | 效果 |
|---|---|
| 你是誰 / 今天天氣如何 / 講個笑話 | 一般對話（答案會唸出+顯示在 TFT） |
| 開紅燈 / 開黃燈 / 開紫燈 / 關燈 | RGBW LED 變色 / 熄滅 |
| 開紅燈轉藍燈再轉綠燈 | 顏色序列（每色 1 秒） |
| 紅燈閃三下 / 藍燈閃爍 | LED 閃爍（可中途「關燈」打斷） |
| 嗶兩聲 | 喇叭嗶聲 |
| 開綠燈嗶兩聲 | 組合動作 |
| 播放音樂 / 放音樂 | 播網路電台 → **按右鍵停止** |
| 重新開始 | 清空對話記憶 |

---

## 🔧 動作 JSON 格式（LLM 輸出 + 關鍵字規則互補）

伺服器把 LLM 回覆裡的動作抽出來，經 HTTP 標頭 `X-Action`（ASCII JSON）傳給 ESP32：

```jsonc
{"led":  {"r":0-255,"g":0-255,"b":0-255,"w":0-255}}   // 恆亮/關燈(全0)
{"led":  {"r":255,"g":0,"b":0,"w":0,"blink":3}}        // 閃爍 N 次
{"seq":  "r,g,b,w;r,g,b,w;..."}                        // 顏色序列 (每步 1 秒)
{"beep": {"n":2}}                                      // 嗶 N 聲
{"music":"play"}                                       // 播放網路電台
```
顏色以伺服器的 `_COLORS` 表為準（比 LLM 可靠）；中文數字（三/兩…）可解析；回覆文字會清掉 markdown ```json 圍欄。

---

## 🌐 Mac 伺服器 HTTP 端點

| 端點 | 說明 |
|---|---|
| `POST /chat` | body=16kHz PCM(chunked)；回 WAV + 標頭 X-STT / X-Reply / X-Action / **X-Mood** |
| `GET /textmono` | 最後回覆的 1-bit 單色點陣(7200 bytes)，供 TFT 逐字顯示 |
| `GET /textimg` | 最後回覆的 240×240 RGB565(115200 bytes)（備用） |
| `GET /music` | 網路電台即時轉 44.1kHz 單聲道 PCM 串流（右鍵中斷連線即停） |
| `GET/POST /reset` | 清空對話記憶 |
| `GET /ping` | 回 `ok`（開機測試連線） |

---

## 🔌 I2S-GO v1.1 腳位

```
INMP441 麥克風 : WS=33, SCK=25, SD=26  (I2S_NUM_0)
MAX98357A 喇叭 : BCLK=13, LRC=12, DIN=15 (I2S_NUM_1; 語音16kHz, 音樂44.1kHz)
ST7789 TFT     : SCLK=18, MOSI=19, CS=5, DC=16, RES=23, BL=4
RGBW LED       : data=32 (SK6812, 用 RMT 不用 I2S)
按鈕           : 左=IO0(說話), 右=IO35(待機→調音量 / 音樂播放中→停止)
```

---

## 🖥️ Mac 端：安裝與啟動

```bash
pip install faster-whisper flask edge-tts imageio-ffmpeg opencc-python-reimplemented numpy Pillow
ollama pull gemma2:9b          # Ollama 桌面 App 或 brew install ollama
```
啟動（edge-tts 在 ~/.local/bin，記得帶進 PATH）：
```bash
cd .../esp32_voice_assistant/mac_brain
PATH="$HOME/.local/bin:$PATH" python3 server.py
```
看到 `serving on http://0.0.0.0:5005` + `whisper ready` 即就緒（首次下載 Whisper small ~500MB）。
模型檔在 `~/.ollama/models`；Mac LAN IP 用 `ipconfig getifaddr en1`（本例 192.168.68.126）。

### server.py 可調參數
| 變數 | 預設 | 說明 |
|---|---|---|
| `OLLAMA_MODEL` | `gemma2:9b` | 換模型 |
| `EDGE_VOICE` | `zh-TW-HsiaoChenNeural` | 換語音（男聲 `zh-TW-YunJheNeural`） |
| `WHISPER_SIZE` | `small` | `medium` 更準較慢 |
| `LAT`/`LON`/`CITY` | 台北 | 你的城市經緯度 |
| `MAX_TURNS` | `6` | 記憶輪數 |
| `RADIO_URL` | SomaFM Groove Salad | 網路電台 |
| `MUSIC_RATE` | `44100` | 音樂取樣率（卡頓可降 32000） |

---

## 📟 ESP32 端：燒錄

**ESP32 core 1.0.6 + UploadSpeed 460800**（這塊板的關鍵）。USB 埠名可能變動（拔插後 -2120↔-2110），先確認：
```bash
ls /dev/cu.usbserial-*
```
```bash
/opt/homebrew/bin/arduino-cli compile --upload -p /dev/cu.usbserial-XXXX \
  --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=460800" \
  ".../esp32_voice_assistant/esp32_voice"
```
韌體開頭要設定（⚠️ WiFi 帳密為明碼）：
```c
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* BRAIN_HOST = "192.168.68.126";   // Mac 的 LAN IP
```
需要的函式庫：TFT_eSPI(Setup100)、NeoPixelBus_by_Makuna。

---

## 🎤 使用
1. Mac 開著、`server.py` 在跑
2. ESP32 連同一個 WiFi、TFT 顯示 **Ready**
3. **按住左鍵說話**（≤10 秒）→ 放開 → 聽回覆 + TFT 逐字顯示
4. 語音控制硬體 / 播音樂（右鍵停）；說「重新開始」清記憶

連線前提：ESP32 與 Mac 在**同一網路**、Mac IP 維持 `192.168.68.126`（會變就設 DHCP 保留，或把 BRAIN_HOST 改成 `Mac-mini.local`）。

---

## ⚠️ 關鍵經驗（這塊板/這個專案踩過的坑）

1. **燒錄必用 460800**；預設 921600 會訊號失真、**靜默刷錄失敗**。
2. **ESP8266Audio/原廠範例要用 ESP32 core 1.0.6**（2.0.17 不相容）；本語音韌體也沿用 1.0.6。
3. **播放要嚴格 2-byte 對齊**：edge-tts→ffmpeg 的 WAV 會塞 LIST/INFO metadata（PCM 從 78 非 44）→ 伺服器加 `-map_metadata -1 -fflags +bitexact` 修成標準 44；ESP32 端改**逐位元組掃 `data` 區塊**才播，否則整段位移→全雜訊。
4. **NeoPixelBus 的 `Neo800KbpsMethod` 在 ESP32 偷用 I2S1**，和喇叭衝突→開機全黑；改用 **`NeoEsp32Rmt0Sk6812Method`（RMT）**。
5. **beep 不可用 `i2s_zero_dma_buffer` 收尾**（會把還沒播的短嗶聲洗掉）；改成送靜音推出 DMA + 等它播完。
6. **LED 動畫執行緒安全**：閃爍/序列用 core 0 任務，但同時只允許一個在控 LED；新指令先 `stopLed()` 等前一個結束，否則多個任務同時 `LED.Show()` → RMT 失控卡死。
7. **動作 JSON 要從口語文字抽乾淨**：LLM 可能用 ```json 圍欄包起來 → 清掉圍欄 + 判斷 led/beep/seq/music 才當動作。
8. **音樂改取樣率**用 `i2s_set_clk(port, rate, bits, ch)`（core 1.0.6 沒有 `i2s_set_sample_rate`）；音樂 44.1kHz、語音 16kHz，播完切回。
9. **IO34~39 無內建上拉**（右鍵 IO35 靠板上外部上拉）；且只能輸入。
10. **硬體接觸**：TFT / MAX98357A / 喇叭排針搬動(含拔插 USB)後易鬆，沒反應先重插再懷疑程式。
11. **無 PSRAM**：故採 push-to-talk 串流、短句；音樂 16-bit 單聲道。

---

## 📂 檔案
```
esp32_voice_assistant/
├── README.md                    (本檔)
├── mac_brain/server.py          Mac 大腦 (STT+LLM+TTS+記憶+即時+渲染+動作+音樂)
└── esp32_voice/esp32_voice.ino  ESP32 韌體 (錄音串流/播放/逐字/LED/嗶聲/音樂)
```

*本機、免費、免金鑰的 ESP32 語音助理。由 Claude 協助建置，2026-10。*

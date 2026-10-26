/*  I2S-GO 語音對話助理 (ESP32 端) — Mac 當大腦
    按住左鍵(IO0)說話 → 邊錄邊「串流上傳」(chunked) 到 Mac (不佔大緩衝, 支援長錄音)
      → Mac 回傳中文語音 WAV → MAX98357A 喇叭播放
    TFT 顯示狀態。 用 core 1.0.6, UploadSpeed=460800。
    Mac 大腦: i2sgo_projects/esp32_voice_assistant/mac_brain/server.py
*/
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <NeoPixelBus.h>
#include "driver/i2s.h"
#include <math.h>

// ===== 設定 =====
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* BRAIN_HOST = "192.168.68.126";
const int   BRAIN_PORT = 5005;
const char* BRAIN_PATH = "/chat";

#define MIC_PORT   I2S_NUM_0
#define SPK_PORT   I2S_NUM_1
#define SAMPLE_RATE 16000
#define REC_MAX_MS  10000        // 最長錄音 10 秒
#define REC_GAIN    6            // 麥克風軟體增益

TFT_eSPI tft = TFT_eSPI(240, 240);
// 用 RMT (不用 I2S, 避免和喇叭 I2S_NUM_1 衝突); SK6812 = RGBW 時序
NeoPixelBus<NeoGrbwFeature, NeoEsp32Rmt0Sk6812Method> LED(1, 32);   // RGBW LED @ GPIO32
int g_pendingBeep = 0;
bool g_pendingMusic = false;

// 待機表情臉狀態
bool faceInit = false;
int pdx = 0, pdy = 0;
unsigned long tMove = 0, tBlink = 0, tMouth = 0;
long nMove = 1000, nBlink = 3000, nMouth = 3000;
#define EYE_CY 92
#define EYE_LX 84
#define EYE_RX 156
#define MX 120
#define MY 188

// ---------- I2S ----------
void micInit() {
  i2s_config_t c = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = 0, .dma_buf_count = 4, .dma_buf_len = 256,
    .use_apll = false, .tx_desc_auto_clear = false, .fixed_mclk = 0
  };
  i2s_pin_config_t p = { .bck_io_num = 25, .ws_io_num = 33,
                         .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = 26 };
  i2s_driver_install(MIC_PORT, &c, 0, NULL);
  i2s_set_pin(MIC_PORT, &p);
}
void spkInit() {
  i2s_config_t c = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = 0, .dma_buf_count = 8, .dma_buf_len = 256,
    .use_apll = false, .tx_desc_auto_clear = true, .fixed_mclk = 0
  };
  i2s_pin_config_t p = { .bck_io_num = 13, .ws_io_num = 12,
                         .data_out_num = 15, .data_in_num = I2S_PIN_NO_CHANGE };
  i2s_driver_install(SPK_PORT, &c, 0, NULL);
  i2s_set_pin(SPK_PORT, &p);
  i2s_zero_dma_buffer(SPK_PORT);
}

// ---------- TFT 狀態 ----------
void status(const char* s, uint16_t color) {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 240, 28, TFT_NAVY);
  tft.setTextFont(2); tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setCursor(6, 6); tft.print("Voice Assistant  (Mac)");
  tft.setTextFont(4); tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(20, 100); tft.print(s);
  tft.setTextFont(2); tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(6, 216); tft.print("Hold [L] to talk (<=10s)");
}

// ---------- 待機表情臉: 兩隻眼睛(會轉會眨) + 嘴巴(各種表情) ----------
#define F_EYE   0xFFFF   // 眼白(白)
#define F_PUPIL 0x2124   // 瞳孔(深灰近黑)
#define F_MOUTH 0xFD20   // 嘴巴(橘)
#define F_BLUSH 0xFB51   // 臉頰紅暈(粉紅)
#define F_BROW  0x4208   // 眉毛(深灰)

int g_mood = 0;          // 心情: 0中性 1開心 2難過 3生氣 4驚訝 5愛心
int g_volIdx = 3;        // 音量檔位 (預設最大)
const int VOL_LV[4] = {25, 50, 75, 100};   // 講話音量 % 檔位

// 畫一隻眼: 眼白圓角矩形 + 瞳孔(偏移 dx,dy 表示看的方向)
void drawEye(int cx, int dx, int dy) {
  tft.fillRoundRect(cx - 30, EYE_CY - 34, 60, 68, 18, F_EYE);
  tft.fillCircle(cx + dx, EYE_CY + dy, 15, F_PUPIL);
  tft.fillCircle(cx + dx + 5, EYE_CY + dy - 5, 4, F_EYE);   // 高光
}

// 眨眼: 先把眼睛變成一條橫線, 停一下, 再張開
void blinkEyes() {
  for (int i = 0; i < 2; i++) {
    int cx = i ? EYE_RX : EYE_LX;
    tft.fillRoundRect(cx - 30, EYE_CY - 34, 60, 68, 18, TFT_BLACK);
    tft.fillRoundRect(cx - 30, EYE_CY - 4, 60, 8, 4, F_EYE);
  }
  delay(110);
  drawEye(EYE_LX, pdx, pdy);
  drawEye(EYE_RX, pdx, pdy);
}

// 畫嘴巴曲線: depth>0 笑(U), =0 平, <0 不開心(∩)
void mouthCurve(int depth) {
  const int W = 40;
  for (int x = -W; x <= W; x++) {
    float t = (float)x / W;
    int y = MY + (int)(depth - depth * t * t);
    tft.fillCircle(MX + x, y, 3, F_MOUTH);
  }
}

// 畫嘴巴表情 (先清掉嘴部區域)
void drawMouth(int expr) {
  tft.fillRect(MX - 50, MY - 34, 100, 72, TFT_BLACK);
  switch (expr) {
    case 0: mouthCurve(20); break;                                    // 開心笑
    case 1: mouthCurve(0); break;                                     // 面無表情(平)
    case 2: tft.fillCircle(MX, MY + 2, 13, F_MOUTH);                  // 嘟嘴
            tft.fillCircle(MX, MY + 2, 6, TFT_BLACK); break;
    case 3: tft.fillEllipse(MX, MY + 4, 15, 22, F_MOUTH);            // 驚訝張嘴(O)
            tft.fillEllipse(MX, MY + 4, 8, 13, TFT_BLACK); break;
    case 4: mouthCurve(-16); break;                                   // 不開心
    case 5: mouthCurve(26);                                           // 大笑(露齒)
            tft.fillRect(MX - 22, MY - 2, 44, 7, F_EYE); break;
    default: mouthCurve(14); break;
  }
}

// 臉頰紅暈 (已移除, 保留空函式避免改動各處呼叫)
void drawBlush(int level) { (void)level; }

// 笑眼 (開心/愛心): 向上彎的弧 ^ ^
void happyEye(int cx) {
  const int W = 26, D = 15;
  for (int x = -W; x <= W; x++) {
    float t = (float)x / W;
    int y = EYE_CY - (int)(D * (1 - t * t));
    tft.fillCircle(cx + x, y, 4, F_EYE);
  }
}

// 睜大眼 (驚訝): 眼白更大、瞳孔更小
void wideEye(int cx, int dx, int dy) {
  tft.fillRoundRect(cx - 33, EYE_CY - 39, 66, 78, 20, F_EYE);
  tft.fillCircle(cx + dx, EYE_CY + dy, 11, F_PUPIL);
  tft.fillCircle(cx + dx + 4, EYE_CY + dy - 4, 3, F_EYE);
}

// 眉毛: 生氣(內低外高, 往鼻子壓) / 難過(內高外低, 八字)
void drawBrow(int cx, bool rightEye, bool angry) {
  int by = EYE_CY - 44;
  int in = rightEye ? cx - 20 : cx + 20;   // 靠鼻側
  int out = rightEye ? cx + 24 : cx - 24;  // 靠外側
  int yIn = angry ? by + 12 : by - 2;
  int yOut = angry ? by - 2 : by + 12;
  tft.fillTriangle(out, yOut, in, yIn, out, yOut + 7, F_BROW);
  tft.fillTriangle(out, yOut + 7, in, yIn, in, yIn + 7, F_BROW);
}

// 依心情畫眼睛(含眉毛), 不畫嘴
void drawMoodEyes(int mood, int dx, int dy) {
  switch (mood) {
    case 1: case 5:                                    // 開心 / 愛心
      happyEye(EYE_LX); happyEye(EYE_RX); break;
    case 4:                                            // 驚訝
      wideEye(EYE_LX, dx, dy); wideEye(EYE_RX, dx, dy); break;
    case 2:                                            // 難過: 眼往下 + 八字眉
      drawEye(EYE_LX, dx, 6); drawEye(EYE_RX, dx, 6);
      drawBrow(EYE_LX, false, false); drawBrow(EYE_RX, true, false); break;
    case 3:                                            // 生氣: 壓眉
      drawEye(EYE_LX, dx, dy); drawEye(EYE_RX, dx, dy);
      drawBrow(EYE_LX, false, true); drawBrow(EYE_RX, true, true); break;
    default:                                           // 中性
      drawEye(EYE_LX, dx, dy); drawEye(EYE_RX, dx, dy); break;
  }
}

// 該心情的紅暈濃度 / 收尾嘴型
int moodBlush(int mood) { return (mood == 1 || mood == 5) ? 2 : (mood == 2 || mood == 3) ? 0 : 1; }
int moodMouth(int mood) {
  switch (mood) { case 2: case 3: return 4; case 4: return 3; default: return 0; }
}

// 從 HTTP 標頭取 X-Mood 設定 g_mood
void parseMood(const String& hdr) {
  int p = hdr.indexOf("X-Mood:");
  if (p < 0) { g_mood = 0; return; }
  String v = hdr.substring(p + 7); v.trim();
  int e = v.indexOf('\r'); if (e >= 0) v = v.substring(0, e);
  v.toLowerCase();
  if      (v.startsWith("happy"))     g_mood = 1;
  else if (v.startsWith("sad"))       g_mood = 2;
  else if (v.startsWith("angry"))     g_mood = 3;
  else if (v.startsWith("surprised")) g_mood = 4;
  else if (v.startsWith("love"))      g_mood = 5;
  else                                g_mood = 0;
}

// 每圈呼叫一次, 非阻塞做待機動畫
void faceStep() {
  if (!faceInit) {
    tft.fillScreen(TFT_BLACK);
    pdx = 0; pdy = 0;
    drawEye(EYE_LX, 0, 0);
    drawEye(EYE_RX, 0, 0);
    drawBlush(1);
    drawMouth(0);
    unsigned long now = millis();
    tMove = tBlink = tMouth = now;
    nMove = random(700, 1600);
    nBlink = random(2600, 5200);
    nMouth = random(2500, 5000);
    faceInit = true;
    return;
  }
  unsigned long now = millis();
  if (now - tBlink > nBlink) {                 // 眨眼
    blinkEyes();
    tBlink = now; nBlink = random(2600, 5200);
  } else if (now - tMove > nMove) {            // 眼睛到處轉 (兩眼同方向)
    pdx = random(-13, 14);
    pdy = random(-15, 16);
    drawEye(EYE_LX, pdx, pdy);
    drawEye(EYE_RX, pdx, pdy);
    tMove = now; nMove = random(700, 1600);
  }
  if (now - tMouth > nMouth) {                 // 換表情
    drawMouth(random(0, 6));
    tMouth = now; nMouth = random(2500, 5000);
  }
}

// 聆聽臉: 直視、睜大眼、微笑專心聽
void faceListening() {
  tft.fillScreen(TFT_BLACK);
  pdx = 0; pdy = 0;
  drawEye(EYE_LX, 0, 0);
  drawEye(EYE_RX, 0, 0);
  drawBlush(1);
  mouthCurve(8);
}

// 思考臉: 眼睛往上看(想事情)、小抿嘴
void faceThinking() {
  tft.fillScreen(TFT_BLACK);
  pdx = 8; pdy = -11;
  drawEye(EYE_LX, pdx, pdy);
  drawEye(EYE_RX, pdx, pdy);
  drawBlush(1);
  tft.fillRoundRect(MX - 12, MY, 24, 6, 3, F_MOUTH);
}

// 說話臉: core 0 任務, 播放期間嘴巴不停開合, 偶爾眨眼
volatile bool g_talk_run = false;
void talkTask(void *p) {
  int mood = g_mood;
  bool canBlink = (mood != 1 && mood != 5);   // 笑眼不眨
  tft.fillScreen(TFT_BLACK);
  drawMoodEyes(mood, 0, 0);
  drawBlush(moodBlush(mood));
  int frame = 0;
  unsigned long tb = millis(); long nb = random(1500, 3000);
  while (g_talk_run) {
    tft.fillRect(MX - 50, MY - 30, 100, 66, TFT_BLACK);   // 清嘴部
    int m = frame & 3;
    if      (m == 0) tft.fillEllipse(MX, MY + 4, 16, 6, F_MOUTH);                                          // 近閉
    else if (m == 2) { tft.fillEllipse(MX, MY + 4, 16, 20, F_MOUTH); tft.fillEllipse(MX, MY + 4, 8, 11, TFT_BLACK); } // 大開(O)
    else             tft.fillEllipse(MX, MY + 4, 15, 13, F_MOUTH);                                         // 半開
    frame++;
    if (canBlink && millis() - tb > nb) {                  // 偶爾眨眼
      for (int i = 0; i < 2; i++) {
        int cx = i ? EYE_RX : EYE_LX;
        tft.fillRoundRect(cx - 33, EYE_CY - 39, 66, 78, 20, TFT_BLACK);
        tft.fillRoundRect(cx - 30, EYE_CY - 4, 60, 8, 4, F_EYE);
      }
      vTaskDelay(pdMS_TO_TICKS(110));
      drawMoodEyes(mood, 0, 0);
      tb = millis(); nb = random(1500, 3000);
    }
    vTaskDelay(pdMS_TO_TICKS(130));
  }
  tft.fillRect(MX - 50, MY - 34, 100, 72, TFT_BLACK);      // 講完收尾嘴型(配合心情)
  drawMouth(moodMouth(mood));
  vTaskDelete(NULL);
}
void startTalking() {
  g_talk_run = true;
  xTaskCreatePinnedToCore(talkTask, "talk", 4096, NULL, 1, NULL, 0);   // core 0
}
void stopTalking() {
  if (g_talk_run) {
    g_talk_run = false;
    delay(200);                                            // 等任務畫完結束
  }
}

// 右鍵: 切換講話音量檔位, 短暫顯示音量條
void showVolume() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(4); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(70, 40); tft.printf("%d%%", VOL_LV[g_volIdx]);
  for (int i = 0; i < 4; i++) {                            // 4 格音量條
    int x = 30 + i * 48, h = 30 + i * 30;
    uint16_t col = (i <= g_volIdx) ? TFT_GREEN : 0x4208;
    tft.fillRoundRect(x, 180 - h, 38, h, 4, col);
  }
  tft.setTextFont(2); tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(60, 210); tft.print("Voice volume");
}
void cycleVolume() {
  g_volIdx = (g_volIdx + 1) % 4;
  showVolume();
  while (digitalRead(35) == LOW) delay(10);               // 等右鍵放開
  faceInit = false;                                        // 回到待機表情臉
}

// ---------- 逐字顯示: 取 Mac 的 1-bit 單色點陣, 用另一核心做打字機效果 ----------
uint8_t g_mono[7200];              // 240x240 1-bit (每列 30 byte, MSB first)
volatile bool g_reveal_run = false;

bool fetchMono() {
  WiFiClient c;
  if (!c.connect(BRAIN_HOST, BRAIN_PORT)) return false;
  c.print("GET /textmono HTTP/1.1\r\n");
  c.printf("Host: %s:%d\r\n", BRAIN_HOST, BRAIN_PORT);
  c.print("Connection: close\r\n\r\n");
  int crlf = 0, got = 0; bool inBody = false;
  unsigned long t0 = millis();
  while ((c.connected() || c.available()) && millis() - t0 < 15000 && got < 7200) {
    if (!c.available()) { if (!c.connected()) break; delay(1); continue; }
    if (!inBody) {
      int ch = c.read(); if (ch < 0) continue;
      if      (ch == '\r' && (crlf == 0 || crlf == 2)) crlf++;
      else if (ch == '\n' && (crlf == 1 || crlf == 3)) crlf++;
      else crlf = 0;
      if (crlf == 4) inBody = true;
    } else {
      int r = c.read(g_mono + got, 7200 - got);
      if (r > 0) got += r; else delay(1);
    }
  }
  c.stop();
  return got == 7200;
}

inline bool monoBit(int x, int y) {
  return (g_mono[y * 30 + (x >> 3)] >> (7 - (x & 7))) & 1;
}

// 在 core 0 執行: 一行一行、左到右逐列把白字寫出來 (打字機效果)
void revealTask(void *p) {
  tft.fillScreen(TFT_BLACK);
  for (int line = 0; line < 7; line++) {
    int y0 = 6 + 33 * line; if (y0 >= 240) break;
    int y1 = min(240, y0 + 33);
    bool any = false;                         // 跳過空白行
    for (int y = y0; y < y1 && !any; y++)
      for (int x = 0; x < 240; x++) if (monoBit(x, y)) { any = true; break; }
    if (!any) continue;
    for (int x = 0; x < 240; x++) {
      for (int y = y0; y < y1; y++)
        if (monoBit(x, y)) tft.drawPixel(x, y, TFT_WHITE);
      if ((x & 3) == 3) vTaskDelay(pdMS_TO_TICKS(10));   // 逐字節奏
    }
  }
  g_reveal_run = false;
  vTaskDelete(NULL);
}

void startReveal() {
  if (fetchMono()) {
    g_reveal_run = true;
    xTaskCreatePinnedToCore(revealTask, "reveal", 4096, NULL, 1, NULL, 0);  // core 0
  }
}

// ---------- 硬體動作: 解析 X-Action JSON -> 控制 LED / 嗶聲 ----------
int jGet(const String& s, const char* key, int def) {
  int p = s.indexOf(key); if (p < 0) return def;
  p = s.indexOf(':', p);  if (p < 0) return def;
  p++; while (p < (int)s.length() && s[p] == ' ') p++;
  int v = 0; bool any = false;
  while (p < (int)s.length() && s[p] >= '0' && s[p] <= '9') { v = v * 10 + (s[p] - '0'); p++; any = true; }
  return any ? v : def;
}

String jGetStr(const String& s, const char* key) {   // 取 "key":"...." 的字串值
  int p = s.indexOf(key); if (p < 0) return "";
  p = s.indexOf(':', p);  if (p < 0) return "";
  p++; while (p < (int)s.length() && s[p] == ' ') p++;
  if (p < (int)s.length() && s[p] == '"') {
    p++; int e = s.indexOf('"', p); if (e < 0) return "";
    return s.substring(p, e);
  }
  return "";
}

// LED 動畫任務 (core 0, RMT, 不干擾 I2S/SPI)。同時只允許一個在控 LED。
volatile bool g_blink_run = false, g_blink_stop = false;
volatile bool g_seq_run = false,   g_seq_stop = false;
uint8_t g_br, g_bg, g_bb, g_bw; int g_blinkCount = 0;
String g_seq;

void blinkTask(void *p) {
  for (int i = 0; i < g_blinkCount && !g_blink_stop; i++) {
    LED.SetPixelColor(0, RgbwColor(g_br, g_bg, g_bb, g_bw)); LED.Show();
    for (int t = 0; t < 25 && !g_blink_stop; t++) vTaskDelay(pdMS_TO_TICKS(10));
    LED.SetPixelColor(0, RgbwColor(0, 0, 0, 0)); LED.Show();
    for (int t = 0; t < 25 && !g_blink_stop; t++) vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!g_blink_stop) { LED.SetPixelColor(0, RgbwColor(g_br, g_bg, g_bb, g_bw)); LED.Show(); }
  g_blink_run = false; vTaskDelete(NULL);
}

// 顏色序列 "r,g,b,w;r,g,b,w;..." 每步停 ~700ms
void seqTask(void *p) {
  int pos = 0;
  while (pos < (int)g_seq.length() && !g_seq_stop) {
    int semi = g_seq.indexOf(';', pos); if (semi < 0) semi = g_seq.length();
    String step = g_seq.substring(pos, semi); pos = semi + 1;
    int v[4] = {0, 0, 0, 0}, vi = 0, sp = 0;
    while (vi < 4) {
      int comma = step.indexOf(',', sp);
      String num = (comma < 0) ? step.substring(sp) : step.substring(sp, comma);
      v[vi++] = num.toInt();
      if (comma < 0) break; sp = comma + 1;
    }
    LED.SetPixelColor(0, RgbwColor(v[0], v[1], v[2], v[3])); LED.Show();
    for (int t = 0; t < 100 && !g_seq_stop; t++) vTaskDelay(pdMS_TO_TICKS(10));  // 每色間隔 1 秒
  }
  g_seq_run = false; vTaskDelete(NULL);
}

// 停掉所有 LED 動畫並等結束 (確保只有一個在控 LED)
void stopLed() {
  if (g_blink_run) { g_blink_stop = true; unsigned long t = millis(); while (g_blink_run && millis() - t < 1500) delay(10); }
  if (g_seq_run)   { g_seq_stop = true;   unsigned long t = millis(); while (g_seq_run   && millis() - t < 2500) delay(10); }
  g_blink_stop = false; g_seq_stop = false;
}

void doAction(const String& a) {
  if (a.indexOf("\"seq\"") >= 0) {                    // 顏色序列
    stopLed();
    g_seq = jGetStr(a, "\"seq\"");
    if (g_seq.length() > 0) { g_seq_run = true; xTaskCreatePinnedToCore(seqTask, "seq", 3072, NULL, 1, NULL, 0); }
  } else if (a.indexOf("\"led\"") >= 0) {
    int r = jGet(a, "\"r\"", 0), g = jGet(a, "\"g\"", 0);
    int b = jGet(a, "\"b\"", 0), w = jGet(a, "\"w\"", 0);
    int blink = jGet(a, "\"blink\"", 0);
    stopLed();
    if (blink > 0) {                                  // 閃爍
      g_br = r; g_bg = g; g_bb = b; g_bw = w; g_blinkCount = blink;
      g_blink_run = true; xTaskCreatePinnedToCore(blinkTask, "blink", 2048, NULL, 1, NULL, 0);
    } else {                                          // 恆亮 / 關燈
      LED.SetPixelColor(0, RgbwColor(r, g, b, w)); LED.Show();
    }
  }
  if (a.indexOf("\"beep\"") >= 0) {                   // 嗶聲延到播放後 (共用喇叭)
    int n = jGet(a, "\"n\"", 1);
    g_pendingBeep = n < 1 ? 1 : (n > 10 ? 10 : n);
  }
  if (a.indexOf("\"music\"") >= 0) g_pendingMusic = true;   // 播放音樂 (回覆後進播放模式)
}

// ---------- 網路電台播放模式: 邊收邊播, 右鍵(IO35)停止 ----------
void playMusic() {
  WiFiClient c;
  if (!c.connect(BRAIN_HOST, BRAIN_PORT)) return;
  c.print("GET /music HTTP/1.1\r\n");
  c.printf("Host: %s:%d\r\n", BRAIN_HOST, BRAIN_PORT);
  c.print("Connection: close\r\n\r\n");
  status("Music  [R]=stop", TFT_MAGENTA);
  i2s_set_clk(SPK_PORT, 44100, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);  // 音樂 44.1kHz
  int crlf = 0; bool inBody = false;
  uint8_t buf[512]; size_t w; int16_t stereo[128 * 2]; int sf = 0; int pendingLow = -1;
  while (c.connected() || c.available()) {
    if (digitalRead(35) == LOW) break;               // 右鍵停止
    if (!c.available()) { if (!c.connected()) break; delay(1); continue; }
    if (!inBody) {
      int ch = c.read(); if (ch < 0) continue;
      if      (ch == '\r' && (crlf == 0 || crlf == 2)) crlf++;
      else if (ch == '\n' && (crlf == 1 || crlf == 3)) crlf++;
      else crlf = 0;
      if (crlf == 4) inBody = true;
    } else {
      int r = c.read(buf, sizeof(buf)); if (r <= 0) { delay(1); continue; }
      for (int i = 0; i < r; i++) {
        if (pendingLow < 0) pendingLow = buf[i];
        else {
          int16_t s = (int16_t)(pendingLow | (buf[i] << 8)); pendingLow = -1;
          stereo[sf * 2] = s; stereo[sf * 2 + 1] = s; sf++;
          if (sf == 128) { i2s_write(SPK_PORT, stereo, 128 * 4, &w, portMAX_DELAY); sf = 0; }
        }
      }
    }
  }
  if (sf > 0) i2s_write(SPK_PORT, stereo, sf * 4, &w, portMAX_DELAY);
  i2s_zero_dma_buffer(SPK_PORT);
  i2s_set_clk(SPK_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);  // 切回 16kHz
  c.stop();
  while (digitalRead(35) == LOW) delay(10);           // 等右鍵放開
  status("Ready", TFT_CYAN);
}

void beep(int n) {
  const int RATE = 16000, FREQ = 880, MS = 120, GAP = 90;
  size_t w; float ph = 0; const float st = 2.0f * PI * FREQ / RATE;
  int16_t buf[256];
  for (int k = 0; k < n; k++) {
    int N = RATE * MS / 1000;
    for (int i = 0; i < N; ) {
      int cnt = min(128, N - i);
      for (int j = 0; j < cnt; j++) {
        int16_t s = (int16_t)(sinf(ph) * 6000 * VOL_LV[g_volIdx] / 100); ph += st; if (ph > 2 * PI) ph -= 2 * PI;
        buf[j * 2] = s; buf[j * 2 + 1] = s;
      }
      i2s_write(SPK_PORT, buf, cnt * 4, &w, portMAX_DELAY); i += cnt;
    }
    int G = RATE * GAP / 1000; memset(buf, 0, sizeof(buf));
    for (int i = 0; i < G; ) { int cnt = min(128, G - i); i2s_write(SPK_PORT, buf, cnt * 4, &w, portMAX_DELAY); i += cnt; }
  }
  // 多送一點靜音把最後的嗶聲推出 DMA, 再等它播完 (不可 zero_dma, 否則洗掉嗶聲)
  memset(buf, 0, sizeof(buf));
  for (int i = 0; i < 2048; i += 128) i2s_write(SPK_PORT, buf, 128 * 4, &w, portMAX_DELAY);
  vTaskDelay(pdMS_TO_TICKS(60));
}

// ---------- 一次對話: 邊錄邊串流上傳 -> 播放回覆 ----------
void converse() {
  WiFiClient client;
  if (!client.connect(BRAIN_HOST, BRAIN_PORT)) {
    status("No server", TFT_RED); delay(1500); return;
  }
  // 送出 chunked POST 標頭
  client.printf("POST %s HTTP/1.1\r\n", BRAIN_PATH);
  client.printf("Host: %s:%d\r\n", BRAIN_HOST, BRAIN_PORT);
  client.print("Content-Type: application/octet-stream\r\n");
  client.print("Transfer-Encoding: chunked\r\n");
  client.print("Connection: close\r\n\r\n");

  // 邊錄邊送 (按住期間, 最長 REC_MAX_MS)
  faceListening();           // 聆聽表情
  i2s_zero_dma_buffer(MIC_PORT);
  int16_t tmp[256]; size_t br;
  unsigned long start = millis();
  long sent = 0;
  while (digitalRead(0) == 0 && millis() - start < REC_MAX_MS) {
    i2s_read(MIC_PORT, tmp, sizeof(tmp), &br, pdMS_TO_TICKS(100));
    int got = br / 2;
    for (int i = 0; i < got; i++) {
      int v = tmp[i] * REC_GAIN;
      if (v > 32767) v = 32767; if (v < -32768) v = -32768;
      tmp[i] = v;
    }
    int bytes = got * 2;
    if (bytes > 0) {
      client.printf("%x\r\n", bytes);      // chunk 大小 (hex)
      client.write((uint8_t*)tmp, bytes);  // chunk 資料
      client.print("\r\n");
      sent += bytes;
    }
  }
  client.print("0\r\n\r\n");               // 結束 chunk

  if (sent < SAMPLE_RATE) {                // 少於 ~0.5 秒就不處理
    faceInit = false;                      // 回到表情臉
    while (client.connected() && client.available()) client.read();
    client.stop(); return;
  }

  // 逐位元組處理回應, 不靠固定長度 (避免網路分段造成的對齊錯位)
  // 階段1: 掃到 HTTP 標頭結束 \r\n\r\n
  // 階段2: 在 WAV 中掃到 "data" 區塊 + 跳過 4 byte size
  // 階段3: 之後都是 PCM, 嚴格 2 byte 一取樣 -> 單聲道複製雙聲道 -> 喇叭
  faceThinking();            // 思考表情
  uint8_t buf[512]; size_t w;
  int16_t stereo[128 * 2]; int sf = 0;
  int phase = 0;              // 0=HTTP標頭, 1=找 data, 2=播 PCM
  int crlf = 0;              // \r\n\r\n 比對狀態
  int dmi = 0;               // "data" 比對狀態
  int sizeSkip = -1;         // data 後的 4 byte size
  int pendingLow = -1;       // 半個取樣
  const char DATA[4] = {'d','a','t','a'};
  String hdr; hdr.reserve(700);   // 累積 HTTP 標頭以取 X-Action
  unsigned long t0 = millis();
  while ((client.connected() || client.available()) && millis() - t0 < 60000) {
    if (!client.available()) { if (!client.connected()) break; delay(1); continue; }
    int r = client.read(buf, sizeof(buf));
    if (r <= 0) continue;
    for (int i = 0; i < r; i++) {
      uint8_t c = buf[i];
      if (phase == 0) {                       // HTTP 標頭
        if (hdr.length() < 700) hdr += (char)c;
        if      (c == '\r' && (crlf == 0 || crlf == 2)) crlf++;
        else if (c == '\n' && (crlf == 1 || crlf == 3)) crlf++;
        else crlf = 0;
        if (crlf == 4) { phase = 1; doAction(hdr); parseMood(hdr); startTalking(); }  // 動作 + 心情 + 說話表情
      } else if (phase == 1) {                 // 找 WAV data 區塊
        if (sizeSkip > 0) {                     // 跳過 data 後的 4 byte size
          sizeSkip--;
          if (sizeSkip == 0) phase = 2;
        } else if (c == DATA[dmi]) {            // 比對 "data"
          dmi++; if (dmi == 4) sizeSkip = 4;
        } else dmi = (c == DATA[0]) ? 1 : 0;
      } else {                                  // PCM
        if (pendingLow < 0) pendingLow = c;
        else {
          int16_t s = (int16_t)(pendingLow | (c << 8)); pendingLow = -1;
          s = (int16_t)((int32_t)s * VOL_LV[g_volIdx] / 100);     // 套用講話音量
          stereo[sf * 2] = s; stereo[sf * 2 + 1] = s; sf++;
          if (sf == 128) { i2s_write(SPK_PORT, stereo, 128 * 4, &w, portMAX_DELAY); sf = 0; }
        }
      }
    }
  }
  if (sf > 0) i2s_write(SPK_PORT, stereo, sf * 4, &w, portMAX_DELAY);
  i2s_zero_dma_buffer(SPK_PORT);
  client.stop();

  stopTalking();                                                     // 播完 -> 嘴巴停下

  if (g_pendingBeep > 0) { beep(g_pendingBeep); g_pendingBeep = 0; }  // 播完後嗶聲

  if (g_pendingMusic) { g_pendingMusic = false; playMusic(); }   // 進網路電台播放模式
}

void setup() {
  delay(500);
  Serial.begin(115200);
  tft.begin(); tft.setRotation(3); tft.fillScreen(TFT_BLACK);
  pinMode(0, INPUT_PULLUP);
  pinMode(35, INPUT_PULLUP);          // 右鍵 (停止音樂)
  tft.setTextFont(2); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(6, 100); tft.print("Connecting WiFi...");

  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t++ < 60) {
    delay(300);
    if (t == 30) { WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
  }
  Serial.printf("WiFi status=%d IP=%s\n", WiFi.status(), WiFi.localIP().toString().c_str());

  micInit(); spkInit();
  LED.Begin(); LED.SetPixelColor(0, RgbwColor(0, 0, 0, 0)); LED.Show();   // LED 先關

  randomSeed(esp_random());
  if (WiFi.status() == WL_CONNECTED) faceInit = false;   // 待機表情臉 (loop 會畫)
  else { tft.fillScreen(TFT_BLACK); tft.setCursor(6,100); tft.print("WiFi failed - check 2.4G"); }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (digitalRead(0) == LOW) {          // 左鍵: 說話
      converse();                         // 聆聽/思考/說話表情
      faceInit = false;                   // 對話結束 -> 回待機表情臉
      delay(200);
    } else if (digitalRead(35) == LOW) {  // 右鍵: 調講話音量
      cycleVolume();
    } else {
      faceStep();                         // 待機: 眼睛到處轉/眨眼, 各種表情
    }
  }
  delay(20);
}

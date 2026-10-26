/*  I2S-GO 網路電台 / Podcast 機
    ESP32 + WiFi + ESP8266Audio(MP3 串流) + MAX98357A + ST7789 TFT
    左鍵(IO0)=下一台   右鍵(IO35)=切換音量
    用 ESP32 core 1.0.6 編譯, UploadSpeed=460800
*/
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include "AudioFileSourceICYStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// ====== 請填入你的 WiFi ======
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
// =============================

// 預載電台 (公開 MP3 串流)
struct Station { const char* name; const char* url; };
Station stations[] = {
  {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
  {"SomaFM Drone Zone",   "http://ice1.somafm.com/dronezone-128-mp3"},
  {"SomaFM Indie Pop",    "http://ice1.somafm.com/indiepop-128-mp3"},
  {"SomaFM Lush",         "http://ice1.somafm.com/lush-128-mp3"},
  {"SomaFM Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3"},
};
const int N_STATIONS = sizeof(stations)/sizeof(stations[0]);

// 音量段 (太大會掉電, 保守)
float volumes[] = {0.2, 0.4, 0.7, 1.0};
const int N_VOL = sizeof(volumes)/sizeof(volumes[0]);
int volIdx = 1;
int curStation = 0;

TFT_eSPI tft = TFT_eSPI(240, 240);
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceICYStream *file = nullptr;
AudioFileSourceBuffer *buff = nullptr;
AudioOutputI2S *out = nullptr;

String nowPlaying = "";
int lastL = HIGH, lastR = HIGH;
unsigned long stoppedAt = 0;   // 串流停止時間, 用於自動重連
String statusMsg = "";
bool needRedraw = true;

// ICY metadata (歌名) 回呼
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *str) {
  String t = String(type);
  if (t == "StreamTitle" || t.indexOf("Title") >= 0) {
    nowPlaying = String(str);
    needRedraw = true;
  }
}
void StatusCallback(void *cbData, int code, const char *str) {
  statusMsg = String(str);
}

void drawUI() {
  tft.fillScreen(TFT_BLACK);
  // 標題列
  tft.fillRect(0, 0, 240, 28, TFT_BLUE);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.setCursor(6, 6);
  tft.print("Net Radio");
  // WiFi 狀態
  tft.setCursor(170, 6);
  tft.print(WiFi.status()==WL_CONNECTED ? "WiFi OK" : "No WiFi");
  // 台號 / 台名
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextFont(4);
  tft.setCursor(6, 40);
  tft.printf("%d/%d", curStation+1, N_STATIONS);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(6, 78);
  tft.print(stations[curStation].name);
  // 正在播放
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(6, 120);
  tft.print("Now:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // 歌名斷行顯示
  String np = nowPlaying.length() ? nowPlaying : "(buffering...)";
  int y = 140;
  while (np.length() && y < 210) {
    tft.setCursor(6, y);
    tft.print(np.substring(0, 28));
    if (np.length() > 28) np = np.substring(28); else np = "";
    y += 18;
  }
  // 音量
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(6, 218);
  tft.printf("Vol %d/%d  [L]next [R]vol", volIdx+1, N_VOL);
}

void startStation(int idx) {
  // 關掉舊的
  if (mp3) { mp3->stop(); delete mp3; mp3=nullptr; }
  if (buff) { delete buff; buff=nullptr; }
  if (file) { delete file; file=nullptr; }
  nowPlaying = "";
  needRedraw = true;
  drawUI();

  file = new AudioFileSourceICYStream(stations[idx].url);
  file->RegisterMetadataCB(MDCallback, nullptr);
  buff = new AudioFileSourceBuffer(file, 8192);   // 8KB 網路緩衝
  buff->RegisterStatusCB(StatusCallback, nullptr);
  mp3 = new AudioGeneratorMP3();
  bool ok = mp3->begin(buff, out);
  Serial.printf("START %s | begin=%d heap=%d\n", stations[idx].url, ok, ESP.getFreeHeap());
}

void setup() {
  delay(800);
  Serial.begin(115200);
  tft.begin();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(6, 100);
  tft.print("Connecting WiFi...");

  pinMode(0, INPUT_PULLUP);   // 左鍵
  pinMode(35, INPUT_PULLUP);  // 右鍵

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) { delay(300); tries++; }

  audioLogger = &Serial;
  out = new AudioOutputI2S();          // I2S port 0
  out->SetPinout(13, 12, 15);          // BCLK, LRC, DIN
  out->SetGain(volumes[volIdx]);

  Serial.printf("WiFi status=%d IP=%s heap=%d\n", WiFi.status(), WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
  if (WiFi.status() == WL_CONNECTED) startStation(curStation);
  else { tft.fillScreen(TFT_BLACK); tft.setCursor(6,100); tft.print("WiFi failed - check SSID/PASS"); }
}

unsigned long lastDraw = 0;
void loop() {
  // 播放; 若停了就記錄時間, 準備自動重連
  static unsigned long decodeCnt=0, lastHb=0;
  if (mp3 && mp3->isRunning()) {
    if (mp3->loop()) { decodeCnt++; }
    else {
      mp3->stop();
      statusMsg = "reconnecting...";
      stoppedAt = millis();
      needRedraw = true;
    }
  } else {
    // 自動重連: 停了 2 秒後重啟目前電台
    if (WiFi.status()==WL_CONNECTED && stoppedAt && millis()-stoppedAt > 2000) {
      stoppedAt = 0;
      startStation(curStation);
    }
  }

  // 邊緣觸發: 只在「按下的瞬間」(HIGH->LOW) 動作
  int L = digitalRead(0);
  int R = digitalRead(35);
  if (lastL==HIGH && L==LOW) {           // 左鍵: 下一台
    curStation = (curStation + 1) % N_STATIONS;
    stoppedAt = 0;
    startStation(curStation);
  }
  if (lastR==HIGH && R==LOW) {           // 右鍵: 音量
    volIdx = (volIdx + 1) % N_VOL;
    if (out) out->SetGain(volumes[volIdx]);
    needRedraw = true;
  }
  if (millis()-lastHb > 3000){ lastHb=millis(); Serial.printf("HB run=%d cnt=%lu heap=%d vol=%.1f np=%s\n", mp3&&mp3->isRunning(), decodeCnt, ESP.getFreeHeap(), volumes[volIdx], nowPlaying.c_str()); }
  lastL = L; lastR = R;

  if (needRedraw && millis() - lastDraw > 250) {
    drawUI();
    needRedraw = false;
    lastDraw = millis();
  }
}

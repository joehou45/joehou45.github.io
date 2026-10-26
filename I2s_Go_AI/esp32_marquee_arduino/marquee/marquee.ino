// I2S-GO ST7789 240x240 跑馬燈 "Claude control" — 直接繪製(乾淨黑底, 無 sprite 殘影)
#include <TFT_eSPI.h>
TFT_eSPI tft = TFT_eSPI(240, 240);

const char* MSG = "Claude control";
const int Y = 104;        // 文字帶上緣
const int BAND_H = 34;    // font4 高度約 26, 留點餘裕
const int STEP = 3;
int x, tw;

void setup() {
  tft.begin();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tw = tft.textWidth(MSG);
  x = 240;
}

void loop() {
  tft.fillRect(0, Y, 240, BAND_H, TFT_BLACK);   // 清整條帶
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(x, Y + 4);
  tft.print(MSG);
  x -= STEP;
  if (x < -tw) x = 240;
  delay(25);
}

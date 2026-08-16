/*
 * meeting cost meter
 *
 * starts silently when a meeting begins
 * displays elapsed time in hours and minutes
 *
 */

#include <LovyanGFX.hpp>

const int buttonPin = 1;
const int ledPin = 8;

LGFX tft;

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
}

unsigned long startTime = 0;
unsigned long currentTime = 0;
unsigned long elapsedTime = 0;

void loop() {
  currentTime = millis();
  if (startTime == 0) {
    // start timer silently
    startTime = currentTime;
    while (digitalRead(buttonPin) == HIGH) {
      // wait for button press
    }
    digitalWrite(ledPin, HIGH);
  } else {
    elapsedTime = currentTime - startTime;
    int hours = elapsedTime / 3600000;
    int minutes = (elapsedTime % 3600000) / 60000;
    tft.fillScreen(LGFX_COLOR_BLACK);
    tft.setCursor(10, 10);
    tft.setTextColor(LGFX_COLOR_WHITE);
    tft.setTextSize(4);
    tft.print(hours);
    tft.print(':');
    tft.print(minutes);
    tft.setCursor(10, 50);
    tft.setTextSize(2);
    tft.print("Meeting Cost Meter");
    tft.setCursor(10, 80);
    tft.print("Elapsed Time:");
  }
  delay(100);
}

/*
 * meeting-cost-meter.ino
 *
 * A simple meeting cost meter for lith.
 */

#include <LovyanGFX.hpp>
#include <Preferences.h>

// Set up the display
#define LGFX_USE_V1
LGFX tft;

// Set up the switches
const int SW1_PIN = 1;
const int SW1_DEBOUNCE_MS = 25;
const int SW1_LONG_PRESS_MS = 800;

// Set up the light
const int LIGHT_PIN = 8;

// Set up the meeting cost meter
int meetingStartMs = 0;
bool meetingRunning = false;

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);
  pinMode(SW1_PIN, INPUT_PULLUP);
  pinMode(LIGHT_PIN, OUTPUT);
  // Initialize the Preferences library
  Preferences.begin("lith", false);
  // Load the meeting start time from Preferences
  meetingStartMs = Preferences.getUInt("meeting-start-ms", 0);
  if (meetingStartMs > 0) {
    meetingRunning = true;
  }
  Preferences.end();
}

void loop() {
  // Check if the meeting start time has been set
  if (meetingRunning) {
    // Calculate the time since the meeting started
    int timeSinceStart = millis() - meetingStartMs;
    // Update the light
    if (timeSinceStart < 60000) {
      // Show a pulsing amber light for the first 60 seconds
      analogWrite(LIGHT_PIN, 128);
      delay(500);
      analogWrite(LIGHT_PIN, 0);
      delay(500);
    } else {
      // Show a steady blue light after 60 seconds
      analogWrite(LIGHT_PIN, 255);
    }
  }
  // Check if the switch has been pressed
  int sw1State = digitalRead(SW1_PIN);
  if (sw1State == LOW) {
    // Debounce the switch
    delay(SW1_DEBOUNCE_MS);
    sw1State = digitalRead(SW1_PIN);
    if (sw1State == LOW) {
      // Check if it's a long press
      if (millis() - lastPressTime > SW1_LONG_PRESS_MS) {
        // Reset the meeting start time
        meetingStartMs = 0;
        meetingRunning = false;
        Preferences.begin("lith", false);
        Preferences.putUInt("meeting-start-ms", 0);
        Preferences.end();
      } else {
        // Start the meeting
        meetingStartMs = millis();
        meetingRunning = true;
        Preferences.begin("lith", false);
        Preferences.putUInt("meeting-start-ms", meetingStartMs);
        Preferences.end();
      }
      lastPressTime = millis();
    }
  }
}

const unsigned long lastPressTime = 0;
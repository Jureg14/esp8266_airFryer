#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#define WEBSERVER_H "fix confict"
#include <WiFiManager.h>
#include <ESPAsyncHTTPUpdateServer.h>
#include <SSD1306.h>
#include <RunningMedian.h>

// --- Hardware Definitions ---
// User specified library constructor
OLED oled(128, 64); 
RunningMedian samples = RunningMedian(10); 
WiFiManager wifiManager;
ESPAsyncHTTPUpdateServer updateServer;
AsyncWebServer server(80);

const int PIN_ENCODER_BUTTON = 13;
const int PIN_ENCODER_A = 0;
const int PIN_ENCODER_B = 2;
const int PIN_ADC = A0;
const int BEEPER = 15;
const int HEATER_PIN = 12;
const int FAN_PIN = 14;

// --- Constants ---
const int sResistor = 235000;
const int thermistor_R_nom = 100000;
const int thermistor_beta_coef = 3974.0;
const int thermistor_nom_temp = 298.15;
const float TEMP_HYSTERESIS = 2.0;

// --- Global Variables ---
volatile int encoderCount = 0; 
unsigned long lastDisplayUpdate = 0;
unsigned long cookStartTime = 0;
bool buttonActive = false;     
bool flipAlertDone = false;

// --- State Machine ---
enum SystemState {
  STATE_SET_TEMP,
  STATE_SET_TIME,
  //STATE_PREHEATING, //not sure if I need this
  STATE_COOKING,
  STATE_DONE
};
SystemState currentState = STATE_SET_TEMP;

struct cooking_vars {
  unsigned long duration_minutes;
  float target_temp;
} cook;

// --- ISR: Encoder Interrupt ---
void IRAM_ATTR updateEncoder() {
  if (digitalRead(PIN_ENCODER_B) == HIGH) {
    encoderCount++;
  } else {
    encoderCount--;
  }
}

// --- Helper: Button Press Detection (Non-Blocking) ---
bool isButtonPressed() {
  if (digitalRead(PIN_ENCODER_BUTTON) == LOW) {
    if (!buttonActive) {
      buttonActive = true;
      delay(20); // Debounce
      return true; 
    }
  } else {
    buttonActive = false;
  }
  return false;
}

// --- Helper: Read Temperature ---
float readTemp() {
  int raw_ADC = analogRead(PIN_ADC);
  samples.add(raw_ADC);
  
  if (samples.getMedian() == 0) return 0.0; // Protect against div/0

  float voltage = samples.getMedian() * (1.0 / 1024.0);
  // Ensure your divider math matches your physical wiring!
  float thermistor_R = (voltage * sResistor) / (3.3 - voltage);
  
  float tempC = ((thermistor_beta_coef * thermistor_nom_temp) / 
                (thermistor_beta_coef + (thermistor_nom_temp * log(thermistor_R / thermistor_R_nom)))) - 273.15;
  return tempC;
}

// --- Helper: Beep ---
void beep(int duration, int freq = 1000) {
  tone(BEEPER, freq, duration);
}

// --- Core Logic: Temperature Control ---
void runControlLoop() {
  float currentTemp = readTemp();
  
  if (currentTemp > 80 || currentState == STATE_COOKING) { // Fan safety threshold
    digitalWrite(FAN_PIN, LOW);
  } else {
    digitalWrite(FAN_PIN, HIGH);
  }

  // Only heat if we are actually in cooking state
  if (currentState == STATE_COOKING) {
    if (currentTemp < cook.target_temp - TEMP_HYSTERESIS) {
      digitalWrite(HEATER_PIN, HIGH);
    } else if (currentTemp > cook.target_temp) {
      digitalWrite(HEATER_PIN, LOW);
    }
  } else {
    digitalWrite(HEATER_PIN, LOW);
  }
}

// --- Display Rendering (Updated for styropyr0/SSD1306) ---
void drawUI(float currentTemp) {
  oled.clearScr(); // Library specific clear

  switch (currentState) {
    case STATE_SET_TEMP:
      oled.print("Set Temp:", 10, 10);
      // Using Big numbers if your library supports valid fonts, otherwise standard print
      oled.print(String((int)cook.target_temp) + " C", 30, 30);
      break;

    case STATE_SET_TIME:
      oled.print("Set Time:", 10, 10);
      oled.print(String((int)cook.duration_minutes) + " Min", 30, 30);
      break;

    case STATE_COOKING: {
      unsigned long elapsed = (millis() - cookStartTime) / 1000;
      unsigned long totalSeconds = cook.duration_minutes * 60;
      long remaining = totalSeconds - elapsed;
      if (remaining < 0) remaining = 0;

      oled.print("Cooking...", 0, 0);
      oled.print("T:" + String((int)currentTemp) + "C", 80, 0);
      
      String timeStr = String(remaining / 60) + ":" + ((remaining % 60 < 10) ? "0" : "") + String(remaining % 60);
      oled.print(timeStr, 30, 30);
      break;
    }

    case STATE_DONE:
      oled.print("DONE!", 40, 30);
      break;
  }
  
  oled.inflate(); // Library specific render command
}

void setup() {
  Serial.begin(115200);
  
  // Library specific init
  oled.begin(); 
  
  pinMode(PIN_ADC, INPUT);
  // NOTE: Changed to INPUT_PULLUP. If you have a physical resistor, change back to INPUT
  pinMode(PIN_ENCODER_BUTTON, INPUT_PULLUP); 
  pinMode(PIN_ENCODER_A, INPUT);
  pinMode(PIN_ENCODER_B, INPUT);
  pinMode(BEEPER, OUTPUT);
  pinMode(HEATER_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), updateEncoder, FALLING);

  wifiManager.autoConnect("AutoConnectAP", "password123");
  server.begin();
  
  // Initial Defaults
  cook.target_temp = 180;
  cook.duration_minutes = 30;
  encoderCount = 180; 
}

void loop() {
  float currentT = readTemp();
  runControlLoop();

  switch (currentState) {
    // --- STEP 1: SELECT TEMPERATURE ---
    case STATE_SET_TEMP:
      if (encoderCount < 0) encoderCount = 0;
      if (encoderCount > 250) encoderCount = 250;
      cook.target_temp = encoderCount;

      if (isButtonPressed()) {
        beep(100);
        currentState = STATE_SET_TIME;
        encoderCount = 30; // Reset encoder for Time
      }
      break;

    // --- STEP 2: SELECT TIME ---
    case STATE_SET_TIME:
      if (encoderCount < 1) encoderCount = 1;
      if (encoderCount > 120) encoderCount = 120;
      cook.duration_minutes = encoderCount;

      if (isButtonPressed()) {
        beep(200);
        cookStartTime = millis();
        flipAlertDone = false;
        currentState = STATE_COOKING;
      }
      break;

    // --- STEP 3: COOKING PROCESS ---
    case STATE_COOKING:
      if (millis() - cookStartTime >= (cook.duration_minutes * 60 * 1000)) {
        beep(1000);
        currentState = STATE_DONE;
      }
      // Halfway Alert 
      if (!flipAlertDone && (millis() - cookStartTime > (cook.duration_minutes * 60 * 1000) / 2)) {
         beep(200); delay(100); beep(200); 
         flipAlertDone = true;
      }
      // Cancel with button
      if (isButtonPressed()) {
        currentState = STATE_SET_TEMP;
        encoderCount = cook.target_temp;
      }
      break;

    // --- STEP 4: FINISHED ---
    case STATE_DONE:
      if (isButtonPressed()) {
        currentState = STATE_SET_TEMP;
        encoderCount = 180;
      }
      break;
  }

  // Update Display periodically
  if (millis() - lastDisplayUpdate > 100) {
    drawUI(currentT);
    lastDisplayUpdate = millis();
  }
}
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#define WEBSERVER_H "fix confict"
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncHTTPUpdateServer.h>
#include <SSD1306Wire.h> 
#include <OLEDDisplayFonts.h>
#include <RunningMedian.h>


// --- Hardware Definitions ---
// Address 0x3c, SDA=4, SCL=5 (Standard for ESP8266)
SSD1306Wire oled(0x3c, 4, 5); 

RunningMedian samples = RunningMedian(10); 
WiFiManager wifiManager;
AsyncWebServer server(80);
ESPAsyncHTTPUpdateServer updateServer;

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
const float TEMP_HYSTERESIS = 10.0;

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

// --- Helper: Button Press ---
bool isButtonPressed() {
  if (digitalRead(PIN_ENCODER_BUTTON) == LOW) {
    if (!buttonActive) {
      buttonActive = true;
      delay(20); 
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
  if (samples.getMedian() == 0) return 0.0;

  float voltage = samples.getMedian() * (1.0 / 1024.0);
  float thermistor_R = (voltage * sResistor) / (3.3 - voltage);
  float tempC = ((thermistor_beta_coef * thermistor_nom_temp) / 
                (thermistor_beta_coef + (thermistor_nom_temp * log(thermistor_R / thermistor_R_nom)))) - 273.15;
  float tempC_adjusted = 1.4853 * tempC + 7.2363;
  return tempC_adjusted;

  //after some manual reading and data logging, i got this:
  //Calibration equation:
  //True Temperature = 1.4853 * Device Reading + 7.2363
}

void beep(int duration, int freq = 1000) {
  tone(BEEPER, freq, duration);
}

// --- Control Logic ---
void runControlLoop() {
  float currentTemp = readTemp();
  
  // Fan Logic
  if (currentTemp > 80 || currentState == STATE_COOKING) { 
    digitalWrite(FAN_PIN, LOW); 
  } else {
    digitalWrite(FAN_PIN, HIGH);
  }

  // Heater Logic
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

// --- Display Rendering ---
void drawUI(float currentTemp) {
  oled.clear(); // Standard clear

  // 1. TOP BAR: IP Address
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  String ipStr = "IP: " + WiFi.localIP().toString();
  oled.drawString(0, 0, ipStr); // syntax: drawString(x, y, text)

  switch (currentState) {
    case STATE_SET_TEMP:
      // Label (Small)
      oled.setFont(ArialMT_Plain_10);
      oled.drawString(0, 20, "Set Temp:");
      
      // Value (Big)
      oled.setFont(ArialMT_Plain_24);
      oled.drawString(20, 32, String((int)cook.target_temp) + " C");
      break;

    case STATE_SET_TIME:
      // Label (Small)
      oled.setFont(ArialMT_Plain_10);
      oled.drawString(0, 20, "Set Time:");
      
      // Value (Big)
      oled.setFont(ArialMT_Plain_24);
      oled.drawString(20, 32, String((int)cook.duration_minutes) + " Min");
      break;

    case STATE_COOKING: {
      unsigned long elapsed = (millis() - cookStartTime) / 1000;
      unsigned long totalSeconds = cook.duration_minutes * 60;
      long remaining = totalSeconds - elapsed;
      if (remaining < 0) remaining = 0;

      // Header info (Small)
      oled.setFont(ArialMT_Plain_10);
      oled.drawString(0, 20, "Cooking...");
      oled.drawString(80, 20, "T:" + String((int)currentTemp) + "C");
      
      // Timer (Big)
      oled.setFont(ArialMT_Plain_24);
      String timeStr = String(remaining / 60) + ":" + ((remaining % 60 < 10) ? "0" : "") + String(remaining % 60);
      oled.drawString(25, 35, timeStr);
      break;
    }

    case STATE_DONE:
      oled.setFont(ArialMT_Plain_24);
      oled.drawString(25, 25, "DONE!");
      break;
  }
  
  oled.display(); // Standard render command
}

void setup() {
  Serial.begin(115200);
  
  // ThingPulse init
  oled.init(); 
  oled.flipScreenVertically();
  
  pinMode(PIN_ADC, INPUT);
  pinMode(PIN_ENCODER_BUTTON, INPUT_PULLUP); 
  pinMode(PIN_ENCODER_A, INPUT);
  pinMode(PIN_ENCODER_B, INPUT);
  pinMode(BEEPER, OUTPUT);
  pinMode(HEATER_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), updateEncoder, FALLING);

  wifiManager.autoConnect("AirFryerAP", "password123");
  
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String msg = "System Online.\nTemperature: " + String(readTemp()) + " C";
    request->send(200, "text/plain", msg);
  });

  updateServer.setup(&server);
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
    case STATE_SET_TEMP:
      if (encoderCount < 0) encoderCount = 0;
      if (encoderCount > 250) encoderCount = 250;
      cook.target_temp = encoderCount;

      if (isButtonPressed()) {
        beep(100);
        currentState = STATE_SET_TIME;
        encoderCount = 30; 
      }
      break;

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

    case STATE_COOKING:
      if (millis() - cookStartTime >= (cook.duration_minutes * 60 * 1000)) {
        beep(1000);
        currentState = STATE_DONE;
      }
      if (!flipAlertDone && (millis() - cookStartTime > (cook.duration_minutes * 60 * 1000) / 2)) {
         beep(200); delay(100); beep(200); 
         flipAlertDone = true;
      }
      if (isButtonPressed()) {
        currentState = STATE_SET_TEMP;
        encoderCount = cook.target_temp;
      }
      break;

    case STATE_DONE:
      if (isButtonPressed()) {
        currentState = STATE_SET_TEMP;
        encoderCount = 180;
      }
      break;
  }

  if (millis() - lastDisplayUpdate > 100) {
    drawUI(currentT);
    lastDisplayUpdate = millis();
  }
}
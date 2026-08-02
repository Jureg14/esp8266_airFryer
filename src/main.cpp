#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <RunningMedian.h>
#include <Wire.h>

// --- Hardware Definitions ---
// Pro Micro hardware I2C: SDA=2, SCL=3
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

RunningMedian samples = RunningMedian(10);

const int PIN_ENCODER_BUTTON = 4;
const int PIN_ENCODER_A = 2;
const int PIN_ENCODER_B = 3;
const int PIN_ADC = A2;
const int BEEPER = 15;
const int HEATER_PIN = 9;
const int FAN_PIN = 8;

// --- Constants ---
const long sResistor = 235000;
const long thermistor_R_nom = 100000;
const float thermistor_beta_coef = 3974.0;
const float thermistor_nom_temp = 298.15;
const float HEATER_HYSTERESIS = 10.0;
const float FAN_HYSTERESIS = 5.0;
const float FAN_TEMP_THRESHOLD = 80.0;

// --- Global Variables ---
volatile int encoderCount = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long cookStartTime = 0;
bool buttonActive = false;
bool flipAlertDone = false;

// --- State Machine ---
enum SystemState { STATE_SET_TEMP, STATE_SET_TIME, STATE_COOKING, STATE_DONE };
SystemState currentState = STATE_SET_TEMP;

struct cooking_vars {
  unsigned long duration_minutes;
  float target_temp;
} cook;

// --- ISR: Encoder Interrupt ---
void updateEncoder() {
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
  if (samples.getMedian() == 0)
    return 0.0;

  // 3.3V--------|sResistor|-----adc------|thermistor|--------GND
  //  Pro Micro: 10-bit ADC with internal 1.1V reference
  float voltage = samples.getMedian() * (1.1 / 1024.0);
  float thermistor_R =
      (voltage * sResistor) / (3.3 - voltage); // circuit voltage is 3.3V
  float tempC =
      ((thermistor_beta_coef * thermistor_nom_temp) /
       (thermistor_beta_coef +
        (thermistor_nom_temp * log(thermistor_R / thermistor_R_nom)))) -
      273.15;

  // Single linear calibration derived from 10 empirical probe measurements (R²
  // = 0.9991, MSE = 0.667)
  float tempC_adjusted = 0.76711f * tempC - 16.121f;
  return tempC_adjusted;
}

void beep(int duration, int freq = 1000) { tone(BEEPER, freq, duration); }

// --- Control Logic ---
void runControlLoop() {
  float currentTemp = readTemp();

  static bool fanState = false;
  static bool heaterState = false;

  // Fan Logic (active-LOW relay: LOW = ON, HIGH = OFF)
  if (currentState == STATE_COOKING) {
    fanState = true;
  } else {
    if (currentTemp >= FAN_TEMP_THRESHOLD) {
      fanState = true;
    } else if (currentTemp <= (FAN_TEMP_THRESHOLD - FAN_HYSTERESIS)) {
      fanState = false;
    }
  }
  digitalWrite(FAN_PIN, fanState ? LOW : HIGH);

  // Heater Logic (active-HIGH relay: HIGH = ON, LOW = OFF)
  if (currentState == STATE_COOKING) {
    if (currentTemp <= (cook.target_temp - HEATER_HYSTERESIS)) {
      heaterState = true;
    } else if (currentTemp >= (cook.target_temp + HEATER_HYSTERESIS)) {
      heaterState = false;
    }
  } else {
    heaterState = false;
  }
  digitalWrite(HEATER_PIN, heaterState ? HIGH : LOW);
}

// --- Display Rendering ---
void drawUI(float currentTemp) {
  oled.clearDisplay();

  switch (currentState) {
  case STATE_SET_TEMP:
    // Label (Small)
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 20);
    oled.print("Set Temp:");

    // Value (Big)
    oled.setTextSize(3);
    oled.setCursor(20, 32);
    oled.print(String((int)cook.target_temp) + " C");
    break;

  case STATE_SET_TIME:
    // Label (Small)
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 20);
    oled.print("Set Time:");

    // Value (Big)
    oled.setTextSize(3);
    oled.setCursor(20, 32);
    oled.print(String((int)cook.duration_minutes) + " Min");
    break;

  case STATE_COOKING: {
    unsigned long elapsed = (millis() - cookStartTime) / 1000;
    unsigned long totalSeconds = cook.duration_minutes * 60;
    long remaining = totalSeconds - elapsed;
    if (remaining < 0)
      remaining = 0;

    // Header info (Small)
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 20);
    oled.print("Cooking...");

    // Timer (Big)
    oled.setTextSize(3);
    oled.setCursor(25, 35);
    String timeStr = String(remaining / 60) + ":" +
                     ((remaining % 60 < 10) ? "0" : "") +
                     String(remaining % 60);
    oled.print(timeStr);
    break;
  }

  case STATE_DONE:
    oled.setTextSize(3);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(25, 25);
    oled.print("DONE!");
    break;
  }

  // Always draw current temperature in the top-right corner
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  String tempStr = String((int)currentTemp) + "C";
  int16_t x1, y1;
  uint16_t tw, th;
  oled.getTextBounds(tempStr, 0, 0, &x1, &y1, &tw, &th);
  oled.setCursor(SCREEN_WIDTH - tw, 20);
  oled.print(tempStr);

  oled.display();
}

void setup() {
  Serial.begin(115200);

  // Adafruit SSD1306 init (address 0x3C)
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ; // halt
  }
  oled.clearDisplay();
  oled.display();

  pinMode(PIN_ENCODER_BUTTON, INPUT_PULLUP);
  pinMode(PIN_ENCODER_A, INPUT);
  pinMode(PIN_ENCODER_B, INPUT);
  pinMode(BEEPER, OUTPUT);
  pinMode(HEATER_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), updateEncoder, FALLING);

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
    if (encoderCount < 0)
      encoderCount = 0;
    if (encoderCount > 250)
      encoderCount = 250;
    cook.target_temp = encoderCount;

    if (isButtonPressed()) {
      beep(100);
      currentState = STATE_SET_TIME;
      encoderCount = 30;
    }
    break;

  case STATE_SET_TIME:
    if (encoderCount < 1)
      encoderCount = 1;
    if (encoderCount > 120)
      encoderCount = 120;
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
    if (!flipAlertDone &&
        (millis() - cookStartTime > (cook.duration_minutes * 60 * 1000) / 2)) {
      beep(200);
      delay(100);
      beep(200);
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
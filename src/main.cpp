//Libraries
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#define WEBSERVER_H "fix confict"
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncHTTPUpdateServer.h>
#include <LittleFS.h>
//#include <ArduinoJson.h>
#include <RunningMedian.h>
#include <SSD1306.h>

OLED oled(128, 64);
RunningMedian samples = RunningMedian(5);
WiFiManager wifiManager;
ESPAsyncHTTPUpdateServer updateServer;
AsyncWebServer server(80);

// pinout def
const int PIN_ENCODER_BUTTON = 13;
const int PIN_ENCODER_A = 14;
const int PIN_ENCODER_B = 12;
const int PIN_ADC = 0;
const int BEEPER = 15;
const int oled_sda = 4;
const int oled_scl = 5;
const int heater_pin = 11;
const int fan_pin = 7;

// const values
const int sResistor = 235000;
const int thermistor_R_nom = 100000; // 100k thermistor
const int thermistor_beta_coef = 3974.0;
const int thermistor_nom_temp = 298.15;//273;

volatile int encoderPos = 0;
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 100; // Update every 100ms

float readTemp() {
  //takes the adc, filters and then converts to ºC
  int n_reads = 0;
  //filters the raw ADC values

    int raw_ADC = analogRead(PIN_ADC);
    samples.add(raw_ADC);
    n_reads++;

  n_reads = 0;
  int ADC = samples.getMedian(); 
  //converts the ADC from the esp8266 to a voltage value (yes the ADC goes from 0v to 1v)
  float voltage = ADC * (1.0/ 1024);
  //calculates the thermistor resistance (ohms) with a reorganized voltage divider equation
  float thermistor_R = (voltage*sResistor)/(3.3-voltage);

  float tempC = ((thermistor_beta_coef * thermistor_nom_temp) / (thermistor_beta_coef + (thermistor_nom_temp * log(thermistor_R / thermistor_R_nom))))-273.15;
  return tempC;
}

void measureTemp(){
  float temp = readTemp();
  if(temp>80){
    digitalWrite(fan_pin,HIGH);
  } else {
    digitalWrite(fan_pin,LOW);
  }
  return;
}

void beep(){
  tone(BEEPER,100);
}

void updateLCD(){
  if (millis() - lastDisplayUpdate < DISPLAY_UPDATE_INTERVAL) {
    return;  // Don't update too frequently
  }
  lastDisplayUpdate = millis();

    oled << "aaaaaAAaaaaa" << 64 << 32; // Print "Hello World!" at the center of the screen
    oled.inflate(); // Render the items on the display
    oled.print(String(encoderPos) + " Hello, World!", 0, 0);
}


void cook(){
  //tone(BEEPER,100);
}

void IRAM_ATTR updateEncoder(){
  // Read the current state of the DT pin
  int dtValue = digitalRead(PIN_ENCODER_B);
  
  // If DT state is HIGH, we moved Counter-Clockwise
  // If DT state is LOW, we moved Clockwise
  // (Note: You might need to swap ++ and -- depending on your specific hardware)
  if (dtValue == HIGH) {
    encoderPos++;
  } else {
    encoderPos--;
  }
}

void setup() {
  Serial.begin(115200);
  oled.begin();
  pinMode(PIN_ADC,INPUT);
  pinMode(PIN_ENCODER_BUTTON,INPUT);
  pinMode(PIN_ENCODER_A,INPUT);
  pinMode(PIN_ENCODER_B,INPUT);
  pinMode(BEEPER,OUTPUT);
  pinMode(BEEPER,OUTPUT);
  pinMode(BEEPER,OUTPUT);

  attachInterrupt(PIN_ENCODER_A, updateEncoder, FALLING);

  wifiManager.autoConnect("AutoConnectAP", "password123");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Hello, world");
    });

  server.begin();
}

void loop() {
  measureTemp();
  updateLCD();
  cook();
}

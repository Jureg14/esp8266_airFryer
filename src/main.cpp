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
//WiFiManager wifiManager;
//ESPAsyncHTTPUpdateServer updateServer;
//AsyncWebServer server(80);

// pinout def
const int PIN_ENCODER_BUTTON = 13;
const int PIN_ENCODER_A = 0;
const int PIN_ENCODER_B = 2;
const int PIN_ADC = A0;
const int BEEPER = 15;
const int oled_sda = 4;
const int oled_scl = 5;
const int heater_pin = 12;
const int fan_pin = 14;
//the lower pins of the esp8266-12e must not be used (6 to 11)

// const values
const int sResistor = 235000;
const int thermistor_R_nom = 100000; // 100k thermistor
const int thermistor_beta_coef = 3974.0;
const int thermistor_nom_temp = 298.15;//273;
const int temp_histerisis = 10;

volatile int encoderPos = 0;
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 100; // Update every 100ms
const int Display_timeout = 30000;
bool buttonActive = false;     // For debouncing
bool flipAlertDone = false;

//variables related to cooking (use it for quick recipes?)
struct cooking_vars{
  unsigned long cook_time;
  float target_temp;
  bool flip;
}; 
cooking_vars cook = {30, 200, false}; //generic


float readTemp() {
  //takes the adc, filters and then converts to ºC
  int raw_ADC = analogRead(PIN_ADC);
  samples.add(raw_ADC);
  int ADC = samples.getMedian(); 
  //converts the ADC from the esp8266 to a voltage value (yes the ADC goes from 0v to 1v)
  float voltage = ADC * (1.0/ 1024);
  //calculates the thermistor resistance (ohms) with a reorganized voltage divider equation
  float thermistor_R = (voltage*sResistor)/(3.3-voltage);
  //equation that takes the resistance of the thermistor and returns a temperature
  float tempC = ((thermistor_beta_coef * thermistor_nom_temp) / (thermistor_beta_coef + (thermistor_nom_temp * log(thermistor_R / thermistor_R_nom))))-273.15;
  return tempC;
}
// Helper: Button Press Detection (Non-Blocking)
bool isButtonPressed() {
  if (digitalRead(PIN_ENCODER_BUTTON) == LOW) {
    if (!buttonActive) {
      buttonActive = true;
      delay(20); // Tiny debounce
      return true; // Trigger once
    }
  } else {
    buttonActive = false;
  }
  return false;
}

void controlTemp(float targetTemp)
{
  float temp = readTemp();
  if(temp>80){
    digitalWrite(fan_pin,HIGH);
  } else {
    digitalWrite(fan_pin,LOW);
  }

  if (targetTemp < temp + temp_histerisis){
    digitalWrite(heater_pin,HIGH);
  } else if (targetTemp > temp + temp_histerisis){
    digitalWrite(heater_pin,LOW);
  }

  return;
}

void beep(int duration,int freq = 1000){
  tone(BEEPER, freq, duration);
}

bool tempSet = false;
bool timeSet = false;

void updateLCD(){
  if (millis() - lastDisplayUpdate < DISPLAY_UPDATE_INTERVAL) {
    return;  // Don't update too frequently
  }
  lastDisplayUpdate = millis();
  //menu logic goes here: set temperature, then time, then start cooking.
  //once cooking, show the remaining time and current temperature.
  //once everything is working add a timeout for the display to prevent burn in of the oled.
  if (digitalRead(PIN_ENCODER_BUTTON)==LOW){
    
    while (tempSet == false)
    {
      if (encoderPos < 0) encoderPos = 0; //prevent negative temperatures
      cook.target_temp = encoderPos;
      oled.print(String(encoderPos) + " C", 30, 30);
      oled.inflate();
      if (digitalRead(PIN_ENCODER_BUTTON)==LOW) tempSet = true;
    }
    encoderPos = 0;
    beep(100);
  }
  if (tempSet == true && timeSet == false){
    while (timeSet == false){
      if (encoderPos < 0) encoderPos = 0; //prevent negative time
      cook.cook_time = encoderPos;
      oled.print(String(encoderPos) + " T", 30, 30);
      oled.inflate();
      if (digitalRead(PIN_ENCODER_BUTTON)==LOW) tempSet = true;
    }
    encoderPos = 0;
    beep(100);
  }

  oled.clearScr();
  //  oled.print(String(encoderPos) + " C", 30, 0);
  //  oled.print(String(readTemp()) + " C", 30, 45);
  //  oled.rectangle(15, 15, 100, 30, 5, 2, false); // Draw a rectangle at (15, 15) with width 100, height 30, 5px corner radius, and 2px thickness
  //  oled << "Hello World!" << 30 << 25; // Print "Hello World!" inside the rectangle
  //  oled.inflate(); // Render the items on the display
}


void cook_cicle()
{
  //for now it locks the program in a while loop until the cooking is done, may change to a state machine later.
  beep(100);
  bool flipped;
  unsigned long cook_time_start = millis();
  while(millis() - cook_time_start < cook.cook_time*1000)
  {
    controlTemp(cook.target_temp);
    updateLCD();
    //warns the user to flip halfway through cooking
    if(cook.flip == true && flipped == false && millis() - cook_time_start > (cook.cook_time*500))
    {
      beep(1000,2000);
      beep(500,2000);
      beep(1300,2000);
      flipped = true;
    }
  }
}

void IRAM_ATTR updateEncoder()
{
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

void setup() 
{
  Serial.begin(115200);
  oled.begin();
  pinMode(PIN_ADC,INPUT);
  pinMode(PIN_ENCODER_BUTTON,INPUT);
  pinMode(PIN_ENCODER_A,INPUT);
  pinMode(PIN_ENCODER_B,INPUT);
  pinMode(BEEPER,OUTPUT);
  pinMode(heater_pin,OUTPUT);
  pinMode(fan_pin,OUTPUT);

  attachInterrupt(PIN_ENCODER_A, updateEncoder, FALLING);

  //wifiManager.autoConnect("AutoConnectAP", "password123");

  //server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
  //      request->send(200, "text/plain", "Hello, world");
  //  });

  //server.begin();
}

void loop() 
{
  //Serial.println("Got Here");
  updateLCD();
  //cook_cicle();
  delay(100);
}

/*
    Video: https://www.youtube.com/watch?v=oCMOYS71NIU
    Based on Neil Kolban example for IDF: https://github.com/nkolban/esp32-snippets/blob/master/cpp_utils/tests/BLE%20Tests/SampleNotify.cpp
    Ported to Arduino ESP32 by Evandro Copercini
   Create a BLE server that, once we receive a connection, will send periodic notifications.
   The service advertises itself as: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
   Has a characteristic of: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E - used for receiving data with "WRITE"
   Has a characteristic of: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E - used to send data with  "NOTIFY"
   The design of creating the BLE server is:
   1. Create a BLE Server
   2. Create a BLE Service
   3. Create a BLE Characteristic on the Service
   4. Create a BLE Descriptor on the characteristic
   5. Start the service.
   6. Start advertising.
   In this example rxValue is the data received (only accessible inside that function).
   And txValue is the data to be sent, in this example just a byte incremented every second.
*/
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEProp.h>
#include <SimpleKalmanFilter.h>
#include <Arduino_KNN.h>
#include <Wire.h>
#include "Motor.h"
#include "Haptics.h"
#include "Preferences.h"

BLEServer *pServer = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;
uint8_t txValue = 0;

int velo0pin = 15;
int velo1pin = 4;

int potPin = 26;
int currentPin = 27;
volatile int potReading = 0;
volatile int currentReading = 0;

// Timer setup
#define TIMER0_INTERVAL_US 50

void IRAM_ATTR TimerHandler0();
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
hw_timer_t *timer = NULL;

Motor motor(25, 33, &potReading, &currentReading);
Haptics haptics(2);

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

#define SERVICE_UUID_TEST1 "1a6a0dc9-7db0-4e5f-8b48-5122af7d0b73"
#define CHARACTERISTIC_UUID_TEST1 "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define SERVICE_UUID_TEST3 "6e400005-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_TEST3 "6e400006-b5a3-f393-e0a9-e50e24dcca9e"
#define SERVICE_UUID_VIBCONF "6e400007-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_VIBCONF "6e400008-b5a3-f393-e0a9-e50e24dcca9e"
#define SERVICE_UUID_VELO1 "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_VELO1 "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
#define SERVICE_UUID_VELO2 "6e400009-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_VELO2 "6e40000a-b5a3-f393-e0a9-e50e24dcca9e"
#define SERVICE_UUID_VELO2 "6e40000b-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_VELO2 "6e40000d-b5a3-f393-e0a9-e50e24dcca9e"

BLEProp test1(SERVICE_UUID_TEST1, CHARACTERISTIC_UUID_TEST1, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE, 4);
BLEProp test3(SERVICE_UUID_TEST3, CHARACTERISTIC_UUID_TEST3, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE, 4);
BLEProp velo1(SERVICE_UUID_VELO1, CHARACTERISTIC_UUID_VELO1, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE, 4);
BLEProp velo2(SERVICE_UUID_VELO2, CHARACTERISTIC_UUID_VELO2, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE, 4);
BLEProp vibConf(SERVICE_UUID_VIBCONF, CHARACTERISTIC_UUID_VIBCONF, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE, 20);

Preferences preferences;

// Headers
int doBrian(bool debug_prints);
void calib_switch();
float adc2v(int adc_val);

// Brian globals
// TODO Fix all of these. All are from arduino nano
// Pins
const int buttonPin = 2;
const int buttonPin2 = 3;
const int calib_button_pin = 4; // D4
// const int potPin = 1; we both used pot pin
int LED_pin = 22; // esp32 onboard

const int Hall_1_Pin = 25;
const int Hall_2_Pin = 26;
const int Hall_3_Pin = 27;
const int Hall_4_Pin = 15;
const int Hall_5_Pin = 14;
const int Hall_6_Pin = 4;
const int Hall_7_Pin = 33;

// States
int buttonState = 0;
int buttonState2 = 0;
int calib_button_push = 0;
enum State
{
  standby,
  calibration,
  motoring
};
// State s = standby;
State s = calibration;

// Global Consts
const int sensor_num = 6;
const int interval_duration = 2000; // ms
const int intervals = 4;
const int samples_per_interval = 125;
const int time_per_sample = interval_duration / samples_per_interval;
const int calib_duration = interval_duration * intervals;
const int total_samples = intervals * samples_per_interval;
float averaged_data[samples_per_interval][sensor_num] = {0};

// GLobal Vars
int calib_i = 0;

// Timers
// TODO Refactor these with drews timer
unsigned long start_time;
unsigned long last_time;
unsigned long current_time;
unsigned long elapsed_time;

// Kalman Filter
SimpleKalmanFilter pressureKalmanFilter(1, 1, 0.01);
// KNN Clasifier
KNNClassifier myKNN(6); // same as sensor_num

void setup()
{
  Serial.begin(115200);
  uint8_t vibConfStart[] = {
      255,
      255,
      0,
      0,
      0,
      127,
      63,
      31};
  preferences.begin("vibro", false);
  // preferences.getBytes("vibConf", vibConfStart, 8U);
  preferences.putBytes("vibConf", vibConfStart, sizeof(vibConfStart));

  // Create the BLE Device
  BLEDevice::init("Prosthesis");
  // BLEDevice::setMTU(64); // Wish this worked but I don't think it does on windows
  pServer = BLEDevice::createServer();

  test1.attach(pServer);
  test3.attach(pServer);
  vibConf.attach(pServer);
  velo1.attach(pServer);
  velo2.attach(pServer);

  Serial.println("Server initialized");

  // Start advertising
  pServer->getAdvertising()->setScanResponse(true);
  pServer->getAdvertising()->start();

  vibConf.setBytes((uint8_t *)&vibConfStart, 8);

  test1.setValue(100.0);
  test3.setValue(0.0);
  velo1.setValue(0.0);
  velo2.setValue(0.0);

  vibConf.notify();
  velo1.notify();
  velo2.notify();
  test1.notify();
  test3.notify();

  Serial.println("Waiting a client connection to notify...");

  // Brian Setup
  pinMode(buttonPin, INPUT);
  pinMode(buttonPin2, INPUT);
  pinMode(calib_button_pin, INPUT);
  pinMode(LED_pin, OUTPUT);

  // Kalman Filter Setup

  // Setup timer
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &TimerHandler0, true);
  timerAlarmWrite(timer, 500, true);
  timerAlarmEnable(timer);
}

void loop()
{
  if (deviceConnected)
  {
    float time = (float)millis();
    // Serial.println(time);
    motor.position(test3.getFloat());
    test1.setValue(potReading);
    test1.notify();
    velo1.setValue((float)65.7 * powf(analogReadMilliVolts(velo0pin) / 1000.0, -1.35));
    velo1.notify();
    velo2.setValue((float)65.7 * powf(analogReadMilliVolts(velo1pin) / 1000.0, -1.35));
    velo2.notify();
    test3.notify();
    vibConf.notify();
    delay(10); // bluetooth stack will go into congestion if too many packets are sent
  }

  // disconnecting
  if (!deviceConnected && oldDeviceConnected)
  {
    delay(500); // give the bluetooth stack the chance to get things ready
    pServer->disconnect(0);
    pServer->getAdvertising()->setScanResponse(true);
    pServer->startAdvertising(); // restart advertising
    Serial.println("start advertising");
    oldDeviceConnected = deviceConnected;
    Serial.println("Client Disconnected");
  }
  // connecting
  if (deviceConnected && !oldDeviceConnected)
  {
    // do stuff here on connecting
    oldDeviceConnected = deviceConnected;
    Serial.println("Client Connected");
  }

  for (int i = 0; i < 3; i++)
  {
    if (vibConf.getData()[2 + i] == 1)
    {
      uint8_t *vibUpdate = vibConf.getData();
      vibUpdate[2 + i] = 0;
      vibConf.setBytes(vibUpdate, 8);
      haptics.vibeselect(i);
      Serial.print("go on haptic #");
      Serial.println(i);
      haptics.drv->go();
    }

    if ((float)65.7 * powf(analogReadMilliVolts(velo0pin) / 1000.0, -1.35) > 300)
    {
      haptics.vibeselect(0);
      haptics.drv->go();
    }
    if ((float)65.7 * powf(analogReadMilliVolts(velo1pin) / 1000.0, -1.35) > 300)
    {
      haptics.vibeselect(1);
      haptics.drv->go();
    }
  }

  int pos = doBrian(true);
}

// Currently Runs at 20khz, see #define TIMER0_INTERVAL_US        50
void IRAM_ATTR TimerHandler0()
{
  portENTER_CRITICAL_ISR(&timerMux);
  potReading = analogRead(potPin);
  currentReading = analogRead(currentPin);
  portEXIT_CRITICAL_ISR(&timerMux);
}

// Brian helper
float adc2v(int adc_val)
{
  float source_voltage = 4.3;
  int resolution = 1023;
  float voltage = (adc_val * source_voltage) / (resolution);
  return voltage;
}

void calib_switch()
{
  Serial.println("switching to calibration");
  s = calibration;
  start_time = millis();
  last_time = millis();
  calib_i = 0;

  digitalWrite(LED_pin, HIGH);
  delay(interval_duration * .05);
  digitalWrite(LED_pin, LOW);
  delay(interval_duration * .45);

  digitalWrite(LED_pin, HIGH);
  delay(interval_duration * .05);
  digitalWrite(LED_pin, LOW);
  delay(interval_duration * .45);

  digitalWrite(LED_pin, HIGH);
  delay(interval_duration * .05);
  digitalWrite(LED_pin, LOW);
  delay(interval_duration * .45);
}

int doBrian(bool debug_prints)
{
  static unsigned long lastTime = micros();
  if (micros() - lastTime > 2000000)
  {
    Serial.print("Brian Interval: ");
    Serial.println(micros() - lastTime);
    lastTime = micros();
  }

  // long unsigned summeded_data[samples_per_interval][6];

  switch (s)
  {
  case standby:
    Serial.println("standby");
    calib_button_push = digitalRead(calib_button_pin);
    if (calib_button_push)
    {
      if (debug_prints)
      {
        Serial.println("PUSHED");
      }
      calib_switch();
    }
    break;

  case calibration:
    // calibrate, storing sum in averaged_data
    if (calib_i < (samples_per_interval * intervals))
    {
      // delay sampling to achive desired rate
      if (millis() < (elapsed_time + time_per_sample))
      {
        // Serial.print("delayed");
        // delay(1);
      }
      else
      {
        bool opening = (int)floor((calib_i) / samples_per_interval) % 2 == 0;
        int pos = 0;
        if (opening)
        {
          pos = calib_i % samples_per_interval;
          digitalWrite(LED_pin, HIGH);
          Serial.print("Calibrating at pos ");
          Serial.print(pos);
          Serial.println("/125 while opening");
        }
        else
        { // count position backwards if closing
          pos = (samples_per_interval - 1) - calib_i % samples_per_interval;
          digitalWrite(LED_pin, LOW);
          Serial.print("Calibrating at pos ");
          Serial.print(pos);
          Serial.println("/125 while closing");
        }

        current_time = millis();
        elapsed_time = current_time - start_time;

        // int reading = 420 + calib_i % samples_per_interval;
        // int hall_1 = reading;
        // int hall_2 = reading;
        // int hall_3 = reading;
        // int hall_4 = reading;
        // int hall_5 = reading;
        // int hall_6 = reading;

        int hall_1 = analogRead(Hall_1_Pin);
        int hall_2 = analogRead(Hall_2_Pin);
        int hall_3 = analogRead(Hall_3_Pin);
        int hall_4 = analogRead(Hall_4_Pin);
        int hall_5 = analogRead(Hall_5_Pin);
        int hall_6 = analogRead(Hall_6_Pin);

        // sum readings with respect to position
        averaged_data[pos][0] += hall_1 / intervals;
        averaged_data[pos][1] += hall_2 / intervals;
        averaged_data[pos][2] += hall_3 / intervals;
        averaged_data[pos][3] += hall_4 / intervals;
        averaged_data[pos][4] += hall_5 / intervals;
        averaged_data[pos][5] += hall_6 / intervals;

        if (debug_prints) // Print current reading
        {
          Serial.print("Elapsed_time: ");
          Serial.print(elapsed_time);
          Serial.print(". calib_i: ");
          Serial.print(calib_i);
          Serial.print(". opening: ");
          Serial.print(opening);
          Serial.print(". Position: ");
          Serial.println(pos);

          Serial.print("Current reading: ");
          Serial.print(hall_1);
          Serial.print(", ");
          Serial.print(hall_2);
          Serial.print(", ");
          Serial.print(hall_3);
          Serial.print(", ");
          Serial.print(hall_4);
          Serial.print(", ");
          Serial.print(hall_5);
          Serial.print(", ");
          Serial.println(hall_6);

          Serial.print("Current reading/intervals: ");
          Serial.print(hall_1 / intervals);
          Serial.print(", ");
          Serial.print(hall_2 / intervals);
          Serial.print(", ");
          Serial.print(hall_3 / intervals);
          Serial.print(", ");
          Serial.print(hall_4 / intervals);
          Serial.print(", ");
          Serial.print(hall_5 / intervals);
          Serial.print(", ");
          Serial.println(hall_6 / intervals);

          Serial.print("Current averaged_data: ");
          Serial.print(averaged_data[pos][0]);
          Serial.print(", ");
          Serial.print(averaged_data[pos][1]);
          Serial.print(", ");
          Serial.print(averaged_data[pos][2]);
          Serial.print(", ");
          Serial.print(averaged_data[pos][3]);
          Serial.print(", ");
          Serial.print(averaged_data[pos][4]);
          Serial.print(", ");
          Serial.println(averaged_data[pos][5]);

          delay(1);
        }

        calib_i++;
      }
    }
    else
    {
      if (debug_prints)
      {
        Serial.println("Making KNN");
      }
      //print average entri`es
      for (int b = 0; b < samples_per_interval; b++)
      {

        if (debug_prints)
        {
          Serial.print("Average entry at position ");
          Serial.print(b + 1);
          Serial.print("/");
          Serial.print(samples_per_interval);
          Serial.print(": ");
          for (int a = 0; a < 7; a++)
          {
            Serial.print(averaged_data[b][a]);
            Serial.print(", ");
          }
          Serial.println("");
        }
        myKNN.addExample(averaged_data[b], b);
      }
      Serial.println("Done calibrating.");
      if (debug_prints)
      {
        Serial.print("myKNN.getCount() = ");
        Serial.println(myKNN.getCount());
        Serial.println();
        delay(1000);
      }
      s = motoring; // TODO make motoring
      Serial.print("Calibrated, now returning estimated pos. ");
      digitalWrite(LED_pin, LOW);
      break;
    case motoring:
      int hall_1 = analogRead(Hall_1_Pin);
      int hall_2 = analogRead(Hall_2_Pin);
      int hall_3 = analogRead(Hall_3_Pin);
      int hall_4 = analogRead(Hall_4_Pin);
      int hall_5 = analogRead(Hall_5_Pin);
      int hall_6 = analogRead(Hall_6_Pin);

      // K Nearest Neighbor
      int k = 5;
      float input[] = {adc2v(hall_1), adc2v(hall_2), adc2v(hall_3), adc2v(hall_4), adc2v(hall_5), adc2v(hall_6)};
      int position = myKNN.classify(input, k); // classify input with K=3
      float confidence = myKNN.confidence();
      float estimated_pos = pressureKalmanFilter.updateEstimate(position);

      if (debug_prints)
      {
        Serial.println("Motoring. ");

        Serial.print("Current reading: ");
        Serial.print(hall_1);
        Serial.print(", ");
        Serial.print(hall_2);
        Serial.print(", ");
        Serial.print(hall_3);
        Serial.print(", ");
        Serial.print(hall_4);
        Serial.print(", ");
        Serial.print(hall_5);
        Serial.print(", ");
        Serial.println(hall_6);

        Serial.print("KNN position = ");
        Serial.print(position);

        // Serial.print("confidence = ");
        // Serial.println(confidence);

        Serial.print(" KF estimated_pos = ");
        Serial.print(estimated_pos);

        // delay(1000);
      }
      // TODO use confidence to set noise (q)
      Serial.print(" Target Position:");
      Serial.println(estimated_pos);
      return estimated_pos;
    }

    calib_button_push = digitalRead(calib_button_pin);
    if (calib_button_push)
    {
      calib_switch();
    }
    break;
  }
  return 0;
}
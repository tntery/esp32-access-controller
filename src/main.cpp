#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "secrets.h"

// define variables
const int LED_PROCESSING = GPIO_NUM_12;
const int LED_REJECTED = GPIO_NUM_13;
const int LED_AUTHORIZED = GPIO_NUM_14;
const int BUZZER = GPIO_NUM_25;
const int MAGLOCK_RELAY = GPIO_NUM_26;
const byte WIEGAND_D0_PIN = GPIO_NUM_32;
const byte WIEGAND_D1_PIN = GPIO_NUM_33;

const uint8_t WIEGAND_26_BITS = 26;
const uint8_t WIEGAND_34_BITS = 34;
const uint8_t WIEGAND_MAX_BITS = 64;
const uint32_t WIEGAND_FRAME_GAP_US = 2500;

volatile uint8_t WIEGAND_BIT_BUFFER[WIEGAND_MAX_BITS];
volatile uint8_t WIEGAND_BIT_COUNT = 0;
volatile uint32_t WIEGAND_LAST_BIT_US = 0;

portMUX_TYPE WIEGAND_MUX = portMUX_INITIALIZER_UNLOCKED;

int short CURRENT_LED_REJECTED_STATE = LOW;

// WiFi credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Backend API endpoint
const char* apiUrl = API_URL;

void IRAM_ATTR pushWiegandBit(uint8_t bit) {
  const uint32_t now = micros();

  portENTER_CRITICAL_ISR(&WIEGAND_MUX);
  if (WIEGAND_BIT_COUNT < WIEGAND_MAX_BITS) {
    WIEGAND_BIT_BUFFER[WIEGAND_BIT_COUNT++] = bit;
    WIEGAND_LAST_BIT_US = now;
  } else {
    // Drop the frame if overflow occurs to avoid processing corrupt data.
    WIEGAND_BIT_COUNT = 0;
    WIEGAND_LAST_BIT_US = 0;
  }
  portEXIT_CRITICAL_ISR(&WIEGAND_MUX);
}

void IRAM_ATTR onWiegandD0() {
  pushWiegandBit(0);
}

void IRAM_ATTR onWiegandD1() {
  pushWiegandBit(1);
}

uint32_t bitsToUint32(const uint8_t *bits, uint8_t startInclusive, uint8_t endInclusive) {
  uint32_t value = 0;
  for (uint8_t i = startInclusive; i <= endInclusive; i++) {
    value = (value << 1) | bits[i];
  }
  return value;
}

bool decodeWiegand26(const uint8_t *bits, uint32_t& cardNumber, uint8_t& facilityCode, uint16_t& cardCode) {
  uint8_t firstHalfOnes = 0;
  uint8_t secondHalfOnes = 0;

  for (uint8_t i = 1; i <= 12; i++) {
    firstHalfOnes += bits[i];
  }

  for (uint8_t i = 13; i <= 24; i++) {
    secondHalfOnes += bits[i];
  }

  const bool evenOk = ((firstHalfOnes + bits[0]) % 2) == 0;
  const bool oddOk = ((secondHalfOnes + bits[25]) % 2) == 1;
  if (!evenOk || !oddOk) {
    return false;
  }

  facilityCode = (uint8_t)bitsToUint32(bits, 1, 8);
  cardCode = (uint16_t)bitsToUint32(bits, 9, 24);
  cardNumber = bitsToUint32(bits, 1, 24);
  return true;
}

bool decodeWiegand34(const uint8_t *bits, uint32_t& cardNumber) {
  uint8_t firstHalfOnes = 0;
  uint8_t secondHalfOnes = 0;

  for (uint8_t i = 1; i <= 16; i++) {
    firstHalfOnes += bits[i];
  }

  for (uint8_t i = 17; i <= 32; i++) {
    secondHalfOnes += bits[i];
  }

  const bool evenOk = ((firstHalfOnes + bits[0]) % 2) == 0;
  const bool oddOk = ((secondHalfOnes + bits[33]) % 2) == 1;
  if (!evenOk || !oddOk) {
    return false;
  }

  cardNumber = bitsToUint32(bits, 1, 32);
  return true;
}

bool decodeWiegandWithFallback(const uint8_t *bits, uint8_t bitCount, uint32_t& cardNumber) {
  if (bitCount == WIEGAND_26_BITS) {
    uint8_t facilityCode = 0;
    uint16_t cardCode = 0;

    const bool valid26 = decodeWiegand26(bits, cardNumber, facilityCode, cardCode);
    if (!valid26) {
      Serial.println("Invalid Wiegand-26 parity check");
      return false;
    }

    Serial.print("Wiegand-26 facility code: ");
    Serial.print(facilityCode);
    Serial.print(", card code: ");
    Serial.println(cardCode);
    return true;
  }

  if (bitCount == WIEGAND_34_BITS) {
    const bool valid34 = decodeWiegand34(bits, cardNumber);
    if (!valid34) {
      Serial.println("Invalid Wiegand-34 parity check");
      return false;
    }

    Serial.println("Valid Wiegand-34 frame");
    return true;
  }

  // Fallback for uncommon reader formats: keep data path alive.
  if (bitCount == 32) {
    cardNumber = bitsToUint32(bits, 0, 31);
    Serial.println("Fallback decode: treating 32-bit frame as raw card ID");
    return true;
  }

  if (bitCount > 2 && bitCount <= 34) {
    cardNumber = bitsToUint32(bits, 1, bitCount - 2);

    Serial.print("Fallback decode for ");
    Serial.print(bitCount);
    Serial.println("-bit frame (parity unchecked)");
    return true;
  }

  Serial.print("Ignoring unsupported Wiegand frame length: ");
  Serial.println(bitCount);
  return false;
}

bool readWiegandCard(uint32_t& cardNumber, uint8_t& bitLength) {
  static uint8_t localBits[WIEGAND_MAX_BITS];
  uint8_t localCount = 0;
  bool frameReady = false;

  const uint32_t now = micros();
  portENTER_CRITICAL(&WIEGAND_MUX);
  if (WIEGAND_BIT_COUNT > 0 && (now - WIEGAND_LAST_BIT_US) > WIEGAND_FRAME_GAP_US) {
    localCount = WIEGAND_BIT_COUNT;
    for (uint8_t i = 0; i < localCount; i++) {
      localBits[i] = WIEGAND_BIT_BUFFER[i];
    }
    WIEGAND_BIT_COUNT = 0;
    WIEGAND_LAST_BIT_US = 0;
    frameReady = true;
  }
  portEXIT_CRITICAL(&WIEGAND_MUX);

  if (!frameReady) {
    return false;
  }

  bitLength = localCount;
  return decodeWiegandWithFallback(localBits, localCount, cardNumber);
}

void unlockMaglock() {
  digitalWrite(MAGLOCK_RELAY, HIGH);
  delay(5000);
  digitalWrite(MAGLOCK_RELAY, LOW);
}

void feedbackProcessing() {
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_AUTHORIZED, LOW);
  digitalWrite(LED_PROCESSING, HIGH);
}

void grant() {
  // unlock maglock and provide feedback

  // reset other feedback LEDs
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, LOW);
  CURRENT_LED_REJECTED_STATE = LOW;

  // turn on authorized LED
  digitalWrite(LED_AUTHORIZED, HIGH);

  // sound buzzer
  digitalWrite(BUZZER, HIGH);
  delay(1000);
  digitalWrite(BUZZER, LOW);

  // unlock the maglock for preset time before locking again
  unlockMaglock();

  // turn off authorized LED
  digitalWrite(LED_AUTHORIZED, LOW);
}

void feedbackReject(bool idle = false) {
  // provide feedback for rejected access
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, HIGH);

  CURRENT_LED_REJECTED_STATE = HIGH;

  if (idle) {
    return; // if idle is true, only update LEDs without sounding buzzer
  }

  // sound buzzer with a pattern for rejected access
  for (int i = 0; i < 5; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(80);                      
    digitalWrite(BUZZER, LOW);
    delay(80);
  }

}

void feedbackReset() {
  // reset all feedback indicators
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_AUTHORIZED, LOW);
  digitalWrite(BUZZER, LOW);

  CURRENT_LED_REJECTED_STATE = LOW;
}

void feedbackWiFiConnecting() {
  // provide feedback for WiFi connection in progress
  digitalWrite(LED_PROCESSING, HIGH);
  digitalWrite(LED_REJECTED, HIGH);
  digitalWrite(LED_AUTHORIZED, HIGH);
}

void sendToServer(uint32_t cardNumber) {
  if(WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(apiUrl);
    http.addHeader("Content-Type", "application/json");

    String payload = "{\"access_id\":\"" + String(cardNumber) + "\"}";
    feedbackProcessing();
    int httpResponseCode = http.POST(payload);

    if(httpResponseCode > 0) {
      String response = http.getString();
      Serial.println("Server response: " + response);

      // parse {"access": "GRANT"}


      if(response.indexOf("\"access\": \"GRANT\"") >= 0) {
        Serial.println("Access granted by server");
        grant();
      } else {
        Serial.println("Access denied by server");
        feedbackReject();
      }
    } else {
      Serial.println("Error sending POST: " + String(httpResponseCode));
      feedbackReject();
    }
    http.end();
  } else {
    Serial.println("WiFi not connected");
    feedbackReject();
  }
}

void setup(){
   
  // Assign pin modes
  pinMode(LED_PROCESSING, OUTPUT);
  pinMode(LED_REJECTED, OUTPUT);
  pinMode(LED_AUTHORIZED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(MAGLOCK_RELAY, OUTPUT);
  pinMode(WIEGAND_D0_PIN, INPUT_PULLUP);
  pinMode(WIEGAND_D1_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(WIEGAND_D0_PIN), onWiegandD0, FALLING);
  attachInterrupt(digitalPinToInterrupt(WIEGAND_D1_PIN), onWiegandD1, FALLING);

  // Initialize serial
  Serial.begin(115200);

  // Initialize all LEDs and buzzer to LOW
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_AUTHORIZED, LOW);
  digitalWrite(BUZZER, LOW);
  digitalWrite(MAGLOCK_RELAY, LOW);

  // test all outputs by turning them on for 1 second
  digitalWrite(LED_REJECTED, HIGH);
  delay(1000);
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_PROCESSING, HIGH);
  delay(1000);
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_AUTHORIZED, HIGH);
  delay(1000);
  digitalWrite(LED_AUTHORIZED, LOW);
  digitalWrite(BUZZER, HIGH);
  delay(1000);
  digitalWrite(BUZZER, LOW);
  digitalWrite(MAGLOCK_RELAY, HIGH);
  delay(1000); 
  digitalWrite(MAGLOCK_RELAY, LOW);

 // connect to WiFi

  feedbackWiFiConnecting();

  // print wifi credentials for debugging
  Serial.print("Connecting to WiFi SSID: "); 
  Serial.println(ssid);
  Serial.print("Password: ");
  Serial.println(password);

  // Try connecting
  Serial.print("Connecting to WiFi"); 
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  // reset feedback after successful WiFi connection
  feedbackReset(); 

}

void loop(){
  uint32_t cardNumber = 0;
  uint8_t bitLength = 0;
  if (readWiegandCard(cardNumber, bitLength)) {
    CURRENT_LED_REJECTED_STATE = LOW;

    Serial.print("Valid Wiegand access ID (");
    Serial.print(bitLength);
    Serial.print("-bit): ");
    Serial.println(cardNumber);

    sendToServer(cardNumber);
  }
}
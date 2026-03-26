#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "secrets.h"

// define variables
const int LED_PROCESSING = GPIO_NUM_12;
const int LED_REJECTED = GPIO_NUM_13;
const int LED_AUTHORIZED = GPIO_NUM_14;
const int BUZZER = GPIO_NUM_25;
const int MAGLOCK_RELAY = GPIO_NUM_18;
const int EXIT_BUTTON_PIN = GPIO_NUM_32;
const int TAMPER_SWITCH_PIN = GPIO_NUM_26;
const byte WIEGAND_D0_PIN = GPIO_NUM_32;
const byte WIEGAND_D1_PIN = GPIO_NUM_33;

// control device changeover pins
const int MAGLOCK_PWR_VCC_RALAY = GPIO_NUM_19; 
const int MAGLOCK_PWR_GND_RALAY = GPIO_NUM_21;
const int EXIT_BUTTON_INPUT_PULLUP_RELAY = GPIO_NUM_22;
const int EXIT_BUTTON_INPUT_GND_RELAY = GPIO_NUM_23;

int short CURRENT_LED_REJECTED_STATE = LOW;

// WiFi credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Backend API endpoint
const char* apiUrl = API_URL;

void sendToServer(uint32_t accessId);

////// CHANGEOVER CONTROL LOGIC BELOW //////////
void changeoverControlTo(const char *str) {
  if (strcmp(str, "THIS_DEVICE") == 0) {
    // Connect maglock power to this device and exit button input to this device
    // Energize all control relays to switch connections
    digitalWrite(MAGLOCK_PWR_VCC_RALAY, HIGH);
    digitalWrite(MAGLOCK_PWR_GND_RALAY, HIGH);
    digitalWrite(EXIT_BUTTON_INPUT_PULLUP_RELAY, HIGH);
    digitalWrite(EXIT_BUTTON_INPUT_GND_RELAY, HIGH);
    Serial.println("Switched control to THIS_DEVICE");
  } else if (strcmp(str, "EXTERNAL_CONTROLLER") == 0) {
    // Connect maglock power to external controller and exit button input to external controller
    // De-energize all control relays to switch connections
    digitalWrite(MAGLOCK_PWR_VCC_RALAY, LOW);
    digitalWrite(MAGLOCK_PWR_GND_RALAY, LOW);
    digitalWrite(EXIT_BUTTON_INPUT_PULLUP_RELAY, LOW);
    digitalWrite(EXIT_BUTTON_INPUT_GND_RELAY, LOW);
    Serial.println("Switched control to EXTERNAL_CONTROLLER");
  } else {
    Serial.println("Invalid changeover target specified");
  }
}

////////////////// Wiegand decoding logic below ///

constexpr uint8_t MAX_WIEGAND_BITS = 64;
constexpr uint32_t FRAME_GAP_US = 2500;

volatile uint8_t g_bitBuffer[MAX_WIEGAND_BITS];
volatile uint8_t g_bitCount = 0;
volatile uint32_t g_lastBitMicros = 0;

portMUX_TYPE g_wiegandMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR pushBit(uint8_t bit) {
  const uint32_t now = micros();

  portENTER_CRITICAL_ISR(&g_wiegandMux);
  if (g_bitCount < MAX_WIEGAND_BITS) {
    g_bitBuffer[g_bitCount++] = bit;
    g_lastBitMicros = now;
  } else {
    // Buffer full: reset so we do not keep reporting corrupt frames.
    g_bitCount = 0;
    g_lastBitMicros = 0;
  }
  portEXIT_CRITICAL_ISR(&g_wiegandMux);
}

void IRAM_ATTR onD0Pulse() {
  pushBit(0);
}

void IRAM_ATTR onD1Pulse() {
  pushBit(1);
}

bool checkParity26(const uint8_t *bits) {
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
  return evenOk && oddOk;
}

bool checkParity34(const uint8_t *bits) {
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
  return evenOk && oddOk;
}

uint32_t bitsToUint32(const uint8_t *bits, uint8_t startInclusive, uint8_t endInclusive) {
  uint32_t value = 0;
  for (uint8_t i = startInclusive; i <= endInclusive; i++) {
    value = (value << 1) | bits[i];
  }
  return value;
}

void printRawBits(const uint8_t *bits, uint8_t bitCount) {
  Serial.print("Raw: ");
  for (uint8_t i = 0; i < bitCount; i++) {
    Serial.print(bits[i]);
  }
  Serial.println();
}

void printHex(const uint8_t *bits, uint8_t bitCount) {
  uint8_t nibble = 0;
  uint8_t nibbleBits = 0;

  Serial.print("Hex: 0x");
  for (uint8_t i = 0; i < bitCount; i++) {
    nibble = (nibble << 1) | bits[i];
    nibbleBits++;

    if (nibbleBits == 4) {
      Serial.print(nibble, HEX);
      nibble = 0;
      nibbleBits = 0;
    }
  }

  if (nibbleBits > 0) {
    nibble <<= (4 - nibbleBits);
    Serial.print(nibble, HEX);
  }

  Serial.println();
}

void decodeAndPrint(const uint8_t *bits, uint8_t bitCount) {
  Serial.println();
  Serial.println("=== Wiegand Frame ===");
  Serial.print("Bit count: ");
  Serial.println(bitCount);
  printRawBits(bits, bitCount);
  printHex(bits, bitCount);

  if (bitCount == 26) {
    const uint16_t facilityCode = static_cast<uint16_t>(bitsToUint32(bits, 1, 8));
    const uint16_t accessId = static_cast<uint16_t>(bitsToUint32(bits, 9, 24));
    const bool parityOk = checkParity26(bits);

    Serial.println("Format: 26-bit");
    Serial.print("Facility code: ");
    Serial.println(facilityCode);
    Serial.print("Card number: ");
    Serial.println(accessId);
    Serial.print("Parity: ");
    Serial.println(parityOk ? "OK" : "FAIL");

    sendToServer(accessId);

  } else if (bitCount == 34) {
    const uint32_t accessId = bitsToUint32(bits, 1, 32);
    const bool parityOk = checkParity34(bits);

    Serial.println("Format: 34-bit");
    Serial.print("Card number (32-bit payload): ");
    Serial.println(accessId);
    Serial.print("Parity: ");
    Serial.println(parityOk ? "OK" : "FAIL");
  } else {
    Serial.println("Format: Unknown/Custom");
  }
}

/// end of Wiegand decoding logic //////

/////////////// Main access control logic below ////////////

void unlockMaglock() {
  digitalWrite(MAGLOCK_RELAY, HIGH);
}

void lockMaglock() {
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

  // unlock the maglock for preset time before locking again
  unlockMaglock();

  // sound buzzer
  digitalWrite(BUZZER, HIGH);
  delay(1000);
  digitalWrite(BUZZER, LOW);

  // lock the maglock again after delay
  delay(4000);
  lockMaglock();

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

void handleExitButtonPress() {
  static int lastReading = HIGH;
  static int stableState = HIGH;
  static unsigned long lastDebounceTimeMs = 0;
  const unsigned long debounceDelayMs = 40;

  const int reading = digitalRead(EXIT_BUTTON_PIN);

  if (reading != lastReading) {
    lastDebounceTimeMs = millis();
    lastReading = reading;
  }

  if ((millis() - lastDebounceTimeMs) > debounceDelayMs && reading != stableState) {
    stableState = reading;

    // Active-low button on INPUT_PULLUP: LOW means button pressed.
    if (stableState == LOW) {
      Serial.println("Exit button pressed. Granting egress access.");
      grant();
    }
  }
}

void handleTamperSwitch() {
  static int lastReading = LOW;
  static int stableState = LOW;
  static unsigned long lastDebounceTimeMs = 0;
  static bool tamperAlarmActive = false;
  static bool buzzerState = false;
  static unsigned long lastBuzzerToggleMs = 0;
  const unsigned long debounceDelayMs = 40;
  const unsigned long buzzerToggleIntervalMs = 80;

  const int reading = digitalRead(TAMPER_SWITCH_PIN);

  if (reading != lastReading) {
    lastDebounceTimeMs = millis();
    lastReading = reading;
  }

  if ((millis() - lastDebounceTimeMs) > debounceDelayMs && reading != stableState) {
    stableState = reading;

    // Normally-closed tamper on INPUT_PULLUP: HIGH means contact opened.
    if (stableState == HIGH) {
      Serial.println("Tamper switch opened. Starting buzzer alarm.");
      tamperAlarmActive = true;
      buzzerState = true;
      lastBuzzerToggleMs = millis();
      digitalWrite(BUZZER, HIGH);
    } else {
      Serial.println("Tamper switch restored. Stopping buzzer alarm.");
      tamperAlarmActive = false;
      buzzerState = false;
      digitalWrite(BUZZER, LOW);
    }
  }

  if (tamperAlarmActive && (millis() - lastBuzzerToggleMs) >= buzzerToggleIntervalMs) {
    buzzerState = !buzzerState;
    lastBuzzerToggleMs = millis();
    digitalWrite(BUZZER, buzzerState ? HIGH : LOW);
  }
}

void sendToServer(uint32_t accessId) {
  if(WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(apiUrl);
    http.addHeader("Content-Type", "application/json");

    String payload = "{\"access_id\":\"" + String(accessId) + "\"}";
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
    // handover control back to external controller on WiFi failure
    changeoverControlTo("EXTERNAL_CONTROLLER");
  }
}

void setup(){
   
  // Assign pin modes
  pinMode(LED_PROCESSING, OUTPUT);
  pinMode(LED_REJECTED, OUTPUT);
  pinMode(LED_AUTHORIZED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(MAGLOCK_RELAY, OUTPUT);
  pinMode(EXIT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(TAMPER_SWITCH_PIN, INPUT_PULLUP);
  pinMode(WIEGAND_D0_PIN, INPUT_PULLUP);
  pinMode(WIEGAND_D1_PIN, INPUT_PULLUP);

  // Set changeover control pins to OUTPUT and initialize to default state (connected to external controller) 
  pinMode(MAGLOCK_PWR_VCC_RALAY, OUTPUT);
  pinMode(MAGLOCK_PWR_GND_RALAY, OUTPUT);
  pinMode(EXIT_BUTTON_INPUT_PULLUP_RELAY, OUTPUT);
  pinMode(EXIT_BUTTON_INPUT_GND_RELAY, OUTPUT);
  changeoverControlTo("EXTERNAL_CONTROLLER");

  attachInterrupt(digitalPinToInterrupt(WIEGAND_D0_PIN), onD0Pulse, FALLING);
  attachInterrupt(digitalPinToInterrupt(WIEGAND_D1_PIN), onD1Pulse, FALLING);

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

 // connect to WiFi with feedback
  feedbackWiFiConnecting();

  // print wifi credentials for debugging
  Serial.print("Connecting to WiFi SSID: "); 
  Serial.println(ssid);
  // Serial.print("Password: ");
  // Serial.println(password);

  // Try connecting
  Serial.print("Connecting to WiFi"); 
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  // take note of IP address for debugging
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // takeover control of maglock power and exit button input from external controller on successful WiFi connection
  changeoverControlTo("THIS_DEVICE");

  // reset feedback after successful WiFi connection
  feedbackReset(); 

}

void readWiegandInput() {
  static uint8_t localBits[MAX_WIEGAND_BITS];
  uint8_t localCount = 0;
  bool frameReady = false;

  const uint32_t now = micros();

  portENTER_CRITICAL(&g_wiegandMux);
  if (g_bitCount > 0 && (now - g_lastBitMicros) > FRAME_GAP_US) {
    localCount = g_bitCount;
    for (uint8_t i = 0; i < localCount; i++) {
      localBits[i] = g_bitBuffer[i];
    }
    g_bitCount = 0;
    g_lastBitMicros = 0;
    frameReady = true;
  }
  portEXIT_CRITICAL(&g_wiegandMux);

  if (frameReady) {
    decodeAndPrint(localBits, localCount);
  }
}

void loop(){

  handleExitButtonPress();
  handleTamperSwitch();
  readWiegandInput();

  if (CURRENT_LED_REJECTED_STATE == LOW) {
    digitalWrite(LED_REJECTED, HIGH);
  }

  delay(1);
}
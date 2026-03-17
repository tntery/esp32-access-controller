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
const byte DEMO_WIEGAND_PUSH_BUTTON = GPIO_NUM_32;

volatile bool AUTHENTICATED = false;
int short CURRENT_LED_REJECTED_STATE = LOW;

// WiFi credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Backend API endpoint
const char* apiUrl = API_URL;

void unlockMaglock() {
  digitalWrite(MAGLOCK_RELAY, HIGH);
  delay(5000);
  digitalWrite(MAGLOCK_RELAY, LOW);
}

void feedbackProcessing() {
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

void feedbackReject() {
  // provide feedback for rejected access
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, HIGH);
  tone(BUZZER, 500, 1000); // longer low beep

  CURRENT_LED_REJECTED_STATE = HIGH;
}

void feedbackReset() {
  // reset all feedback indicators
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_AUTHORIZED, LOW);
  noTone(BUZZER);

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
  pinMode(DEMO_WIEGAND_PUSH_BUTTON, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(DEMO_WIEGAND_PUSH_BUTTON), []() {
    AUTHENTICATED = !AUTHENTICATED; // Toggle authentication state for testing
  }, FALLING);

  // Initialize serial
  Serial.begin(115200);

  // Initialize all LEDs and buzzer to LOW
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_AUTHORIZED, LOW);
  digitalWrite(BUZZER, LOW);
  digitalWrite(MAGLOCK_RELAY, LOW);

  // test all outputs by turning them on for 1 second
  digitalWrite(LED_PROCESSING, HIGH);
  delay(1000);
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, HIGH);
  delay(1000);
  digitalWrite(LED_REJECTED, LOW);
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


uint32_t cardNumber = 123456; // Placeholder card number for testing

void loop(){

  // mimic wiegand authentication by reading the state of the AUTHENTICATED pin (active LOW)
  if (AUTHENTICATED) {
    
    // update states
    CURRENT_LED_REJECTED_STATE = LOW;

    Serial.print("Valid card/user ID: ");
    Serial.println(cardNumber);

    // send card number to backend server for validation and access decision
    sendToServer(cardNumber);

    // reset authentication state
    AUTHENTICATED = false;

  } else {

    // only provide feedback id not already provided
    if (CURRENT_LED_REJECTED_STATE == LOW) {

      Serial.println("Unauthorized access attempt detected!");
      feedbackReject();

      CURRENT_LED_REJECTED_STATE = HIGH;
    }

  }
  
}
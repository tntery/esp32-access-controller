#include <Arduino.h>

// define variables
const int LED_PROCESSING = GPIO_NUM_12;
const int LED_REJECTED = GPIO_NUM_13;
const int LED_AUTHORIZED = GPIO_NUM_14;
const int BUZZER = GPIO_NUM_25;
const int MAGLOCK_RELAY = GPIO_NUM_26;
const byte DEMO_WIEGAND_PUSH_BUTTON = GPIO_NUM_32;
volatile bool AUTHENTICATED = false;

int short CURRENT_LED_REJECTED_STATE = LOW;

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

}

uint32_t cardNumber = 123456; // Placeholder card number for testing

void loop(){

  // mimic wiegand authentication by reading the state of the AUTHENTICATED pin (active LOW)
  if (AUTHENTICATED) {
    
    // update states
    CURRENT_LED_REJECTED_STATE = LOW;

    Serial.print("Valid card/user ID: ");
    Serial.println(cardNumber);

    grant(); // directly grant access for testing without backend

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
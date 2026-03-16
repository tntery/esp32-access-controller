#include <Arduino.h>

// define variables
const int LED_PROCESSING = GPIO_NUM_12;
const int LED_REJECTED = GPIO_NUM_13;
const int LED_GRANTED = GPIO_NUM_14;
const int BUZZER = GPIO_NUM_25;
const int MAGLOCK_RELAY = GPIO_NUM_26;
const int AUTHENTICATED = GPIO_NUM_32;

int short CURRENT_LED_REJECTED_STATE = LOW;

void setup(){
   
  // Assign pin modes
  pinMode(LED_PROCESSING, OUTPUT);
  pinMode(LED_REJECTED, OUTPUT);
  pinMode(LED_GRANTED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(MAGLOCK_RELAY, OUTPUT);
  pinMode(AUTHENTICATED, INPUT_PULLUP);

  Serial.begin(115200);

  // Initialize all LEDs and buzzer to LOW
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_GRANTED, LOW);
  digitalWrite(BUZZER, LOW);
  digitalWrite(MAGLOCK_RELAY, LOW);

  // test all outputs by turning them on for 1 second
  digitalWrite(LED_PROCESSING, HIGH);
  delay(1000);
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, HIGH);
  delay(1000);
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_GRANTED, HIGH);
  delay(1000);
  digitalWrite(LED_GRANTED, LOW);
  digitalWrite(BUZZER, HIGH);
  delay(1000);
  digitalWrite(BUZZER, LOW);
  digitalWrite(MAGLOCK_RELAY, HIGH);
  delay(1000); 
  digitalWrite(MAGLOCK_RELAY, LOW);

}

void   loop(){

  // sound buzzer when authenticated goes LOW (active LOW)
  if (digitalRead(AUTHENTICATED) == LOW) { 
    
    // update states
    CURRENT_LED_REJECTED_STATE = 0;

    Serial.println("Authenticated! Unlocking door...");

    digitalWrite(LED_PROCESSING, HIGH); 
    delay(1000);
    digitalWrite(LED_PROCESSING, LOW);

    digitalWrite(LED_GRANTED, HIGH);
    digitalWrite(BUZZER, HIGH);
    digitalWrite(MAGLOCK_RELAY, HIGH);

    delay(2000);

    digitalWrite(MAGLOCK_RELAY, LOW);
    digitalWrite(BUZZER, LOW);
    digitalWrite(LED_GRANTED, LOW);

  } else {

    if (CURRENT_LED_REJECTED_STATE == LOW) {

      // Only turn on the rejected LED and print message if it was not already in the rejected state

      Serial.println("Not authenticated.");

      digitalWrite(LED_REJECTED, HIGH);
      CURRENT_LED_REJECTED_STATE = HIGH;

    }

  }
  
}
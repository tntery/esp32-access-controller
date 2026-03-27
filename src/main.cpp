#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>

#include "secrets.h"

// define variables
const int LED_PROCESSING = GPIO_NUM_12;
const int LED_REJECTED = GPIO_NUM_13;
const int LED_AUTHORIZED = GPIO_NUM_14;
const int BUZZER = GPIO_NUM_25;
const int MAGLOCK_RELAY = GPIO_NUM_18;
const int EXIT_BUTTON_PIN = GPIO_NUM_27;
const int CONFIG_BUTTON_PIN = GPIO_NUM_4;
const int TAMPER_SWITCH_PIN = GPIO_NUM_26;
const byte WIEGAND_D0_PIN = GPIO_NUM_32;
const byte WIEGAND_D1_PIN = GPIO_NUM_33;

// control device changeover pins
const int MAGLOCK_PWR_VCC_RALAY = GPIO_NUM_19; 
const int MAGLOCK_PWR_GND_RALAY = GPIO_NUM_21;
const int EXIT_BUTTON_INPUT_PULLUP_RELAY = GPIO_NUM_22;
const int EXIT_BUTTON_INPUT_GND_RELAY = GPIO_NUM_23;

int short CURRENT_LED_REJECTED_STATE = LOW;

const unsigned long BUTTON_LONG_PRESS_MS = 5000;
const unsigned long CONFIG_RECONNECT_PRESS_MS = 1000;
const unsigned long CONFIG_EXIT_LONG_PRESS_MS = 5000;
const unsigned long CONFIG_MODE_IDLE_TIMEOUT_MS = 180000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

const char *CONFIG_AP_SSID = CONFIG_AP_SSID_DEFAULT;

Preferences g_preferences;
WebServer g_webServer(80);

String g_wifiSsid;
String g_wifiPassword;
String g_apiKey;
String g_configApPassword;
String g_configWebPassword;
bool g_tamperEnabled = true;
bool g_configModeActive = false;
unsigned long g_configModeLastActivityMs = 0;
bool g_webRoutesConfigured = false;
bool g_exitConfigModeRequested = false;

bool g_tamperAlarmActive = false;
bool g_tamperBuzzerState = false;
unsigned long g_lastTamperToggleMs = 0;
bool g_configLedBlinkState = false;
unsigned long g_lastConfigLedBlinkMs = 0;

void sendToServer(uint32_t accessId);
void startConfigMode();
void stopConfigMode();
bool connectToConfiguredWiFi();
void lockMaglock();
void feedbackWiFiConnecting();
void feedbackReject(bool idle);
void feedbackReset();
void handleConfigButtonLongPress();
void updateConfigModeIndicators();

////// CHANGEOVER CONTROL LOGIC BELOW //////////
void changeoverControlTo(const char *str) {
  if (strcmp(str, "THIS_DEVICE") == 0) {
    // Connect maglock power to this device and exit button input to this device
    // Energize all control relays to switch connections
    digitalWrite(MAGLOCK_PWR_VCC_RALAY, HIGH);
    digitalWrite(MAGLOCK_PWR_GND_RALAY, HIGH);
    digitalWrite(EXIT_BUTTON_INPUT_PULLUP_RELAY, HIGH);
    digitalWrite(EXIT_BUTTON_INPUT_GND_RELAY, HIGH);
    // Return LED control to normal runtime feedback flow.
    Serial.println("Switched control to THIS_DEVICE");
  } else if (strcmp(str, "EXTERNAL_CONTROLLER") == 0) {
    // Connect maglock power to external controller and exit button input to external controller
    // De-energize all control relays to switch connections
    digitalWrite(MAGLOCK_PWR_VCC_RALAY, LOW);
    digitalWrite(MAGLOCK_PWR_GND_RALAY, LOW);
    digitalWrite(EXIT_BUTTON_INPUT_PULLUP_RELAY, LOW);
    digitalWrite(EXIT_BUTTON_INPUT_GND_RELAY, LOW);
    // External control indicator: processing and authorized LEDs ON.
    digitalWrite(LED_PROCESSING, HIGH);
    digitalWrite(LED_AUTHORIZED, HIGH);
    Serial.println("Switched control to EXTERNAL_CONTROLLER");
  } else {
    Serial.println("Invalid changeover target specified");
  }
}

String htmlEscape(const String &value) {
  String escaped = value;
  escaped.replace("&", "&amp;");
  escaped.replace("<", "&lt;");
  escaped.replace(">", "&gt;");
  escaped.replace("\"", "&quot;");
  escaped.replace("'", "&#39;");
  return escaped;
}

void loadRuntimeConfig() {
  g_preferences.begin("accesscfg", true);
  g_wifiSsid = g_preferences.getString("ssid", WIFI_SSID);
  g_wifiPassword = g_preferences.getString("pass", WIFI_PASSWORD);
  g_apiKey = g_preferences.getString("api_key", "");
  g_configApPassword = g_preferences.getString("ap_pass", CONFIG_AP_PASSWORD_DEFAULT);
  g_configWebPassword = g_preferences.getString("web_pass", CONFIG_WEB_PASSWORD_DEFAULT);
  g_tamperEnabled = g_preferences.getBool("tamper_en", true);
  g_preferences.end();

  if (g_configApPassword.length() < 8 || g_configApPassword.length() > 63) {
    g_configApPassword = CONFIG_AP_PASSWORD_DEFAULT;
  }

  if (g_configWebPassword.length() < 4 || g_configWebPassword.length() > 63) {
    g_configWebPassword = CONFIG_WEB_PASSWORD_DEFAULT;
  }
}

void saveRuntimeConfig() {
  g_preferences.begin("accesscfg", false);
  g_preferences.putString("ssid", g_wifiSsid);
  g_preferences.putString("pass", g_wifiPassword);
  g_preferences.putString("api_key", g_apiKey);
  g_preferences.putString("ap_pass", g_configApPassword);
  g_preferences.putString("web_pass", g_configWebPassword);
  g_preferences.putBool("tamper_en", g_tamperEnabled);
  g_preferences.end();
}

String buildConfigPageHtml(const String &message) {
  const String checked = g_tamperEnabled ? "checked" : "";
  const String html =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Access Controller Setup</title>"
    "<style>body{font-family:sans-serif;max-width:520px;margin:20px auto;padding:0 12px;}"
    "label{display:block;margin-top:12px;font-weight:600;}"
    "input{width:100%;padding:10px;box-sizing:border-box;}"
    "button{margin-top:16px;padding:10px 14px;}"
    ".msg{padding:10px;background:#eef7ee;border:1px solid #b9deb9;margin:12px 0;}"
    ".row{margin-top:12px;display:flex;align-items:center;gap:10px;}</style></head><body>"
    "<h2>Access Controller Setup</h2>"
    "<p>Device is in setup mode. It returns to normal mode after idle timeout.</p>" +
    (message.length() ? ("<div class='msg'>" + htmlEscape(message) + "</div>") : String("")) +
    "<form method='POST' action='/save'>"
    "<label>WiFi SSID</label><input name='ssid' value='" + htmlEscape(g_wifiSsid) + "' required>"
    "<label>WiFi Password</label><input type='password' name='password' value='" + htmlEscape(g_wifiPassword) + "'>"
    "<label>Setup AP Password (8-63 chars)</label><input type='password' name='ap_password' value='" + htmlEscape(g_configApPassword) + "' minlength='8' maxlength='63' required>"
    "<label>Setup Page Password (4-63 chars)</label><input type='password' name='web_password' value='" + htmlEscape(g_configWebPassword) + "' minlength='4' maxlength='63' required>"
    "<label>API Key</label><input name='api_key' value='" + htmlEscape(g_apiKey) + "'>"
    "<div class='row'><input id='tamper' type='checkbox' name='tamper_enabled' " + checked + ">"
    "<label for='tamper' style='margin:0;font-weight:400;'>Enable tamper detection</label><span>Enable tamper detection</span></div>"
    "<button type='submit'>Save</button></form></body></html>";

  return html;
}

void configureWebRoutes() {
  g_webServer.on("/", HTTP_GET, []() {
    if (!g_webServer.authenticate(CONFIG_WEB_USERNAME, g_configWebPassword.c_str())) {
      return g_webServer.requestAuthentication();
    }

    g_configModeLastActivityMs = millis();
    g_webServer.send(200, "text/html", buildConfigPageHtml(""));
  });

  g_webServer.on("/save", HTTP_POST, []() {
    if (!g_webServer.authenticate(CONFIG_WEB_USERNAME, g_configWebPassword.c_str())) {
      return g_webServer.requestAuthentication();
    }

    g_configModeLastActivityMs = millis();

    g_wifiSsid = g_webServer.arg("ssid");
    g_wifiPassword = g_webServer.arg("password");
    g_apiKey = g_webServer.arg("api_key");
    const String requestedApPassword = g_webServer.arg("ap_password");
    const String requestedWebPassword = g_webServer.arg("web_password");
    g_tamperEnabled = g_webServer.hasArg("tamper_enabled");

    if (requestedApPassword.length() < 8 || requestedApPassword.length() > 63) {
      g_webServer.send(400, "text/html", buildConfigPageHtml("AP password must be 8 to 63 characters."));
      return;
    }

    if (requestedWebPassword.length() < 4 || requestedWebPassword.length() > 63) {
      g_webServer.send(400, "text/html", buildConfigPageHtml("Page password must be 4 to 63 characters."));
      return;
    }

    g_configApPassword = requestedApPassword;
    g_configWebPassword = requestedWebPassword;

    saveRuntimeConfig();
    g_webServer.send(200, "text/html", buildConfigPageHtml("Saved successfully. Leaving setup mode now."));
    g_exitConfigModeRequested = true;
  });
}

bool connectToConfiguredWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);

  // Blink LED_PROCESSING only during connection; all others off.
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_AUTHORIZED, LOW);

  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(g_wifiSsid);
  Serial.print("Connecting to WiFi");

  WiFi.begin(g_wifiSsid.c_str(), g_wifiPassword.c_str());

  bool ledBlinkState = false;
  const unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
    ledBlinkState = !ledBlinkState;
    digitalWrite(LED_PROCESSING, ledBlinkState ? HIGH : LOW);
    delay(500);
    Serial.print(".");
  }
  digitalWrite(LED_PROCESSING, LOW);

  Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    changeoverControlTo("THIS_DEVICE");

    // On successful connection, keep only rejected LED ON.
    digitalWrite(LED_PROCESSING, LOW);
    digitalWrite(LED_AUTHORIZED, LOW);
    digitalWrite(LED_REJECTED, HIGH);
    CURRENT_LED_REJECTED_STATE = LOW;
    return true;
  }

  Serial.println("WiFi connection failed within timeout");
  changeoverControlTo("EXTERNAL_CONTROLLER");

  // On connection failure, show external-control style LEDs.
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_PROCESSING, HIGH);
  digitalWrite(LED_AUTHORIZED, HIGH);
  CURRENT_LED_REJECTED_STATE = LOW;
  return false;
}

void startConfigMode() {
  if (g_configModeActive) {
    return;
  }

  Serial.println("Entering setup mode via long button press");
  g_configModeActive = true;
  g_exitConfigModeRequested = false;
  g_configModeLastActivityMs = millis();
  g_lastConfigLedBlinkMs = millis();
  g_configLedBlinkState = false;

  lockMaglock();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP);

  const bool apStarted = WiFi.softAP(CONFIG_AP_SSID, g_configApPassword.c_str());
  Serial.print("Setup AP SSID: ");
  Serial.println(CONFIG_AP_SSID);
  Serial.print("Setup AP protected: ");
  Serial.println(apStarted ? "YES" : "FAILED");
  Serial.print("Setup portal IP: ");
  Serial.println(WiFi.softAPIP());

  if (!g_webRoutesConfigured) {
    configureWebRoutes();
    g_webRoutesConfigured = true;
  }
  g_webServer.begin();
}

void stopConfigMode() {
  if (!g_configModeActive) {
    return;
  }

  Serial.println("Leaving setup mode");
  g_webServer.stop();
  WiFi.softAPdisconnect(true);
  g_configModeActive = false;
  g_exitConfigModeRequested = false;

  // Leaving setup mode: clear all feedback LEDs before reconnection feedback.
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_AUTHORIZED, LOW);

  connectToConfiguredWiFi();
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
  digitalWrite(LED_PROCESSING, LOW);
  digitalWrite(LED_REJECTED, LOW);
  digitalWrite(LED_AUTHORIZED, LOW);
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

void handleConfigButtonLongPress() {
  static int lastReading = HIGH;
  static int stableState = HIGH;
  static unsigned long lastDebounceTimeMs = 0;
  static unsigned long pressedStartMs = 0;
  static bool longPressHandled = false;
  static unsigned long releasedAtMs = 0;
  static unsigned long pressDurationOnRelease = 0;
  const unsigned long debounceDelayMs = 40;

  const int reading = digitalRead(CONFIG_BUTTON_PIN);

  if (reading != lastReading) {
    lastDebounceTimeMs = millis();
    lastReading = reading;
  }

  if ((millis() - lastDebounceTimeMs) > debounceDelayMs && reading != stableState) {
    stableState = reading;

    if (stableState == LOW) {
      pressedStartMs = millis();
      longPressHandled = false;
      pressDurationOnRelease = 0;
    } else {
      // Button released: capture duration for short-press detection.
      pressDurationOnRelease = millis() - pressedStartMs;
      releasedAtMs = millis();
    }
  }

  const unsigned long pressDurationMs = millis() - pressedStartMs;

  // 1-second press on release: reconnect WiFi if not in config mode and not connected.
  if (stableState == HIGH && !longPressHandled && !g_configModeActive
      && pressDurationOnRelease >= CONFIG_RECONNECT_PRESS_MS
      && pressDurationOnRelease < BUTTON_LONG_PRESS_MS
      && (millis() - releasedAtMs) < 200) {
    pressDurationOnRelease = 0;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Config button 1s press: reconnecting to WiFi.");
      connectToConfiguredWiFi();
    } else {
      Serial.println("Config button 1s press: WiFi already connected, ignoring.");
      changeoverControlTo("THIS_DEVICE"); // ensure control is with this device if already connected
    }
  }

  // Active-low button on INPUT_PULLUP: LOW means button pressed.
  if (stableState == LOW && !longPressHandled && !g_configModeActive && pressDurationMs >= BUTTON_LONG_PRESS_MS) {
    longPressHandled = true;
    startConfigMode();
  }

  if (stableState == LOW && !longPressHandled && g_configModeActive && pressDurationMs >= CONFIG_EXIT_LONG_PRESS_MS) {
    longPressHandled = true;
    Serial.println("Config button long press detected. Leaving setup mode.");
    g_exitConfigModeRequested = true;
  }
}

void updateConfigModeIndicators() {
  const unsigned long blinkIntervalMs = 800;
  if ((millis() - g_lastConfigLedBlinkMs) >= blinkIntervalMs) {
    g_lastConfigLedBlinkMs = millis();
    g_configLedBlinkState = !g_configLedBlinkState;
    digitalWrite(LED_PROCESSING, g_configLedBlinkState ? HIGH : LOW);
    digitalWrite(LED_AUTHORIZED, g_configLedBlinkState ? HIGH : LOW);
  }
}

void handleTamperSwitch() {
  static int lastReading = LOW;
  static int stableState = LOW;
  static unsigned long lastDebounceTimeMs = 0;
  const unsigned long debounceDelayMs = 40;
  const unsigned long buzzerToggleIntervalMs = 80;

  if (!g_tamperEnabled) {
    g_tamperAlarmActive = false;
    g_tamperBuzzerState = false;
    digitalWrite(BUZZER, LOW);
    return;
  }

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
      g_tamperAlarmActive = true;
      g_tamperBuzzerState = true;
      g_lastTamperToggleMs = millis();
      digitalWrite(BUZZER, HIGH);
    } else {
      Serial.println("Tamper switch restored. Stopping buzzer alarm.");
      g_tamperAlarmActive = false;
      g_tamperBuzzerState = false;
      digitalWrite(BUZZER, LOW);
    }
  }

  if (g_tamperAlarmActive && (millis() - g_lastTamperToggleMs) >= buzzerToggleIntervalMs) {
    g_tamperBuzzerState = !g_tamperBuzzerState;
    g_lastTamperToggleMs = millis();
    digitalWrite(BUZZER, g_tamperBuzzerState ? HIGH : LOW);
  }
}

void sendToServer(uint32_t accessId) {
  if(WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(API_URL);
    http.addHeader("Content-Type", "application/json");
    if (g_apiKey.length() > 0) {
      http.addHeader("X-API-Key", g_apiKey);
    }

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
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
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

  loadRuntimeConfig();

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

  connectToConfiguredWiFi();

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
  handleConfigButtonLongPress();

  if (g_configModeActive) {
    updateConfigModeIndicators();
    g_webServer.handleClient();

    if (g_exitConfigModeRequested) {
      stopConfigMode();
      delay(2);
      return;
    }

    if ((millis() - g_configModeLastActivityMs) >= CONFIG_MODE_IDLE_TIMEOUT_MS) {
      stopConfigMode();
    }

    delay(2);
    return;
  }

  handleTamperSwitch();
  readWiegandInput();

  if (CURRENT_LED_REJECTED_STATE == LOW && WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_REJECTED, HIGH);
  }

  delay(1);
}
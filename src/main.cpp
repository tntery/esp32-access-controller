#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include <map>

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

// RC522 custom SPI pin map (uses GPIO16/GPIO17 as requested)
const int RC522_SCK_PIN = GPIO_NUM_17;
const int RC522_MISO_PIN = GPIO_NUM_34;
const int RC522_MOSI_PIN = GPIO_NUM_16;
const int RC522_SS_PIN = GPIO_NUM_23;
const int RC522_RST_PIN = GPIO_NUM_2;

// control device changeover pins
const int MAGLOCK_PWR_VCC_RALAY = GPIO_NUM_19; 
const int MAGLOCK_PWR_GND_RALAY = -1; // relay removed
const int EXIT_BUTTON_INPUT_PULLUP_RELAY = GPIO_NUM_21;
const int EXIT_BUTTON_INPUT_GND_RELAY = GPIO_NUM_22;

int short CURRENT_LED_REJECTED_STATE = LOW;

const unsigned long BUTTON_LONG_PRESS_MS = 5000;
const unsigned long CONFIG_RECONNECT_PRESS_MS = 1000;
const unsigned long CONFIG_EXIT_LONG_PRESS_MS = 5000;
const unsigned long CONFIG_MODE_IDLE_TIMEOUT_MS = 180000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const unsigned long MAPPINGS_REFRESH_INTERVAL_MS = 14400000UL; // 5 minutes

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

struct StringLess {
  bool operator()(const String &lhs, const String &rhs) const {
    return lhs.compareTo(rhs) < 0;
  }
};

std::map<String, bool, StringLess> g_accessMappings; // device_access_id -> true=GRANT, false=REJECT
unsigned long g_lastMappingsFetchMs = 0;

MFRC522 g_mfrc522(RC522_SS_PIN, RC522_RST_PIN);
String g_lastRfidUidHex;
unsigned long g_lastRfidReadMs = 0;

const unsigned long RFID_REPEAT_IGNORE_MS = 1200;

void sendToServer(const String &accessId);
void startConfigMode();
void stopConfigMode();
bool connectToConfiguredWiFi();
void lockMaglock();
void feedbackWiFiConnecting();
void feedbackReject(bool idle);
void feedbackReset();
void handleConfigButtonLongPress();
void updateConfigModeIndicators();
void fetchAccessMappings();
void initRfidReader();
void readRfidInput();
void sendEventToServer(const String &eventAccessId);
String buildEventAccessId(const String &status);

////// CHANGEOVER CONTROL LOGIC BELOW //////////
void changeoverControlTo(const char *str) {
  if (strcmp(str, "THIS_DEVICE") == 0) {
    // Connect maglock power to this device and exit button input to this device
    // Energize all control relays to switch connections
    digitalWrite(MAGLOCK_PWR_VCC_RALAY, HIGH);
    if (MAGLOCK_PWR_GND_RALAY >= 0) {
      digitalWrite(MAGLOCK_PWR_GND_RALAY, HIGH);
    }
    digitalWrite(EXIT_BUTTON_INPUT_PULLUP_RELAY, HIGH);
    digitalWrite(EXIT_BUTTON_INPUT_GND_RELAY, HIGH);
    // Return LED control to normal runtime feedback flow.
    Serial.println("Switched control to THIS_DEVICE");
  } else if (strcmp(str, "EXTERNAL_CONTROLLER") == 0) {
    // Connect maglock power to external controller and exit button input to external controller
    // De-energize all control relays to switch connections
    digitalWrite(MAGLOCK_PWR_VCC_RALAY, LOW);
    if (MAGLOCK_PWR_GND_RALAY >= 0) {
      digitalWrite(MAGLOCK_PWR_GND_RALAY, LOW);
    }
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

void logStringDebug(const char *label, const String &value) {
  Serial.print(label);
  Serial.print(" [");
  Serial.print(value.length());
  Serial.println(" chars]");
  Serial.print("  quoted: >");
  Serial.print(value);
  Serial.println("<");
  Serial.print("  bytes: ");
  for (size_t i = 0; i < value.length(); i++) {
    if (i > 0) {
      Serial.print(' ');
    }
    uint8_t b = static_cast<uint8_t>(value[i]);
    if (b < 16) {
      Serial.print('0');
    }
    Serial.print(b, HEX);
  }
  Serial.println();
}

// Extracts the string value for a JSON key from a small JSON object snippet.
String jsonExtractString(const String &obj, const String &key) {
  const String searchKey = "\"" + key + "\"";
  int keyPos = obj.indexOf(searchKey);
  if (keyPos < 0) return "";
  int colonPos = obj.indexOf(':', keyPos + searchKey.length());
  if (colonPos < 0) return "";
  int valStart = colonPos + 1;
  while (valStart < (int)obj.length() && obj[valStart] == ' ') valStart++;
  if (valStart >= (int)obj.length()) return "";
  if (obj[valStart] == '"') {
    int end = obj.indexOf('"', valStart + 1);
    if (end < 0) return "";
    String extracted = obj.substring(valStart + 1, end);
    extracted.trim();
    return extracted;
  }
  int end = valStart;
  while (end < (int)obj.length() && obj[end] != ',' && obj[end] != '}' && obj[end] != ']') end++;
  String extracted = obj.substring(valStart, end);
  extracted.trim();
  return extracted;
}

void saveMappingsToNvs() {
  String serialized;
  for (auto &entry : g_accessMappings) {
    serialized += entry.first + (entry.second ? ":1," : ":0,");
  }
  if (serialized.endsWith(",")) {
    serialized.remove(serialized.length() - 1);
  }
  g_preferences.begin("accesscfg", false);
  g_preferences.putString("acl_cache", serialized);
  g_preferences.end();
  Serial.print("Saved ");
  Serial.print(g_accessMappings.size());
  Serial.println(" access mappings to NVS.");
}

void loadMappingsFromNvs() {
  g_preferences.begin("accesscfg", true);
  String serialized = g_preferences.getString("acl_cache", "");
  g_preferences.end();

  g_accessMappings.clear();
  if (serialized.length() == 0) return;

  int pos = 0;
  while (pos < (int)serialized.length()) {
    int colonPos = serialized.indexOf(':', pos);
    if (colonPos < 0) break;
    int commaPos = serialized.indexOf(',', colonPos + 1);
    if (commaPos < 0) commaPos = serialized.length();
    String id = serialized.substring(pos, colonPos);
    id.trim();
    bool isGrant = (serialized.charAt(colonPos + 1) == '1');
    g_accessMappings[id] = isGrant;
    pos = commaPos + 1;
  }
  Serial.print("Loaded ");
  Serial.print(g_accessMappings.size());
  Serial.println(" access mappings from NVS.");
}

void parseMappingsFromJson(const String &json) {
  int arrStart = json.indexOf("\"mappings\"");
  if (arrStart < 0) {
    Serial.println("No 'mappings' key in response.");
    return;
  }
  arrStart = json.indexOf('[', arrStart);
  if (arrStart < 0) return;
  int arrEnd = json.lastIndexOf(']');
  if (arrEnd <= arrStart) return;

  std::map<String, bool, StringLess> newMappings;
  int pos = arrStart;
  while (pos < arrEnd) {
    int objStart = json.indexOf('{', pos);
    if (objStart < 0 || objStart >= arrEnd) break;
    int objEnd = json.indexOf('}', objStart);
    if (objEnd < 0 || objEnd > arrEnd) break;

    const String obj = json.substring(objStart, objEnd + 1);
    const String idStr = jsonExtractString(obj, "device_access_id");
    const String accStr = jsonExtractString(obj, "access");

    if (idStr.length() > 0 && accStr.length() > 0) {
      newMappings[idStr] = (accStr == "GRANT");
    }
    pos = objEnd + 1;
  }

  if (!newMappings.empty()) {
    g_accessMappings = std::move(newMappings);
    Serial.print("Parsed ");
    Serial.print(g_accessMappings.size());
    Serial.println(" access mappings.");
  } else {
    Serial.println("No valid mappings parsed; keeping existing cache.");
  }
}

void fetchAccessMappings() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(API_URL);
  if (g_apiKey.length() > 0) {
    http.addHeader("X-API-Key", g_apiKey);
  }

  Serial.println("Fetching access mappings from server...");
  const int code = http.GET();
  if (code == 200) {
    parseMappingsFromJson(http.getString());
    saveMappingsToNvs();
  } else {
    Serial.print("Failed to fetch access mappings, HTTP: ");
    Serial.println(code);
  }
  http.end();
  g_lastMappingsFetchMs = millis();
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

  // Normalize values loaded from NVS to avoid invisible whitespace issues.
  const String apBefore = g_configApPassword;
  const String webBefore = g_configWebPassword;
  g_configApPassword.trim();
  g_configWebPassword.trim();

  bool configUpdated = (g_configApPassword != apBefore) || (g_configWebPassword != webBefore);

  if (g_configApPassword.length() < 8 || g_configApPassword.length() > 63) {
    g_configApPassword = CONFIG_AP_PASSWORD_DEFAULT;
    configUpdated = true;
  }

  if (g_configWebPassword.length() < 4 || g_configWebPassword.length() > 63) {
    g_configWebPassword = CONFIG_WEB_PASSWORD_DEFAULT;
    configUpdated = true;
  }

  if (configUpdated) {
    g_preferences.begin("accesscfg", false);
    g_preferences.putString("ap_pass", g_configApPassword);
    g_preferences.putString("web_pass", g_configWebPassword);
    g_preferences.end();
  }
  loadMappingsFromNvs();
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
  const String apIp = WiFi.softAPIP().toString();
  const String staIp = WiFi.localIP().toString();
  const String staMac = WiFi.macAddress();
  const String apMac = WiFi.softAPmacAddress();
  const String wifiStatus = (WiFi.status() == WL_CONNECTED) ? "Connected" : "Not connected";

  const String deviceInfo =
    "<div style='margin:12px 0;padding:10px;border:1px solid #ddd;background:#f7f7f7;'>"
    "<strong>Device Info</strong>"
    "<div style='margin-top:8px;font-size:14px;line-height:1.5;'>"
    "<div><b>Setup AP SSID:</b> " + htmlEscape(String(CONFIG_AP_SSID)) + "</div>"
    "<div><b>Setup AP IP:</b> " + htmlEscape(apIp) + "</div>"
    "<div><b>Setup AP MAC:</b> " + htmlEscape(apMac) + "</div>"
    "<div><b>STA IP:</b> " + htmlEscape(staIp) + "</div>"
    "<div><b>STA MAC:</b> " + htmlEscape(staMac) + "</div>"
    "<div><b>WiFi Status:</b> " + htmlEscape(wifiStatus) + "</div>"
    "</div></div>";

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
    deviceInfo +
    (message.length() ? ("<div class='msg'>" + htmlEscape(message) + "</div>") : String("")) +
    "<form method='POST' action='/save'>"
    "<label>WiFi SSID</label><input name='ssid' value='" + htmlEscape(g_wifiSsid) + "' required>"
    "<label>WiFi Password</label><input type='password' name='password' value='" + htmlEscape(g_wifiPassword) + "'>"
    "<label>Setup AP Password (8-63 chars, leave blank to keep current)</label><input type='password' name='ap_password' value='' minlength='8' maxlength='63' autocomplete='new-password' placeholder='Leave blank to keep current password'>"
    "<label>Setup Page Password (4-63 chars, leave blank to keep current)</label><input type='password' name='web_password' value='' minlength='4' maxlength='63' autocomplete='new-password' placeholder='Leave blank to keep current password'>"
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
    String requestedApPassword = g_webServer.arg("ap_password");
    String requestedWebPassword = g_webServer.arg("web_password");
    requestedApPassword.trim();
    requestedWebPassword.trim();
    const bool previousTamperEnabled = g_tamperEnabled;
    g_tamperEnabled = g_webServer.hasArg("tamper_enabled");

    if (requestedApPassword.length() > 0 && (requestedApPassword.length() < 8 || requestedApPassword.length() > 63)) {
      g_webServer.send(400, "text/html", buildConfigPageHtml("AP password must be 8 to 63 characters."));
      return;
    }

    if (requestedWebPassword.length() > 0 && (requestedWebPassword.length() < 4 || requestedWebPassword.length() > 63)) {
      g_webServer.send(400, "text/html", buildConfigPageHtml("Page password must be 4 to 63 characters."));
      return;
    }

    if (requestedApPassword.length() > 0) {
      g_configApPassword = requestedApPassword;
    }

    if (requestedWebPassword.length() > 0) {
      g_configWebPassword = requestedWebPassword;
    }

    saveRuntimeConfig();

    if (previousTamperEnabled != g_tamperEnabled) {
      sendEventToServer(buildEventAccessId(g_tamperEnabled ? "TAMPER_CFG_ENABLED" : "TAMPER_CFG_DISABLED"));
    }

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
    fetchAccessMappings();
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
  Serial.println("=== Setup AP startup parameters ===");
  logStringDebug("AP SSID (runtime)", String(CONFIG_AP_SSID));
  logStringDebug("AP password (runtime before trim)", g_configApPassword);
  logStringDebug("AP password (default macro)", String(CONFIG_AP_PASSWORD_DEFAULT));

  String apPassword = g_configApPassword;
  apPassword.trim();
  if (apPassword.length() < 8 || apPassword.length() > 63) {
    apPassword = CONFIG_AP_PASSWORD_DEFAULT;
  }

  logStringDebug("AP password (used by softAP)", apPassword);
  Serial.println("AP channel (default): 1");
  Serial.println("AP hidden (default): 0");
  Serial.println("AP max connections (default): 4");

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  delay(100);

  const bool apStarted = WiFi.softAP(CONFIG_AP_SSID, apPassword.c_str());
  g_configApPassword = apPassword;
  Serial.print("ESP32 STA MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("ESP32 AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());
  Serial.print("Setup AP SSID: ");
  Serial.println(CONFIG_AP_SSID);
  Serial.print("Setup AP protected: ");
  Serial.println(apStarted ? "YES" : "FAILED");
  Serial.print("Setup AP password length: ");
  Serial.println(g_configApPassword.length());
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

    sendToServer(String(accessId));

  } else if (bitCount == 34) {
    const uint32_t accessId = bitsToUint32(bits, 1, 32);
    const bool parityOk = checkParity34(bits);

    Serial.println("Format: 34-bit");
    Serial.print("Card number (32-bit payload): ");
    Serial.println(accessId);
    Serial.print("Parity: ");
    Serial.println(parityOk ? "OK" : "FAIL");

    sendToServer(String(accessId));
  } else {
    Serial.println("Format: Unknown/Custom");
  }
}

/// end of Wiegand decoding logic //////

String uidToHexString(const MFRC522::Uid &uid) {
  String hex;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) {
      hex += "0";
    }
    hex += String(uid.uidByte[i], HEX);
  }
  hex.toUpperCase();
  return hex;
}

void initRfidReader() {
  SPI.begin(RC522_SCK_PIN, RC522_MISO_PIN, RC522_MOSI_PIN, RC522_SS_PIN);
  g_mfrc522.PCD_Init();
  delay(50);

  Serial.println("RC522 initialized");
  Serial.print("  SCK=");
  Serial.println(RC522_SCK_PIN);
  Serial.print("  MISO=");
  Serial.println(RC522_MISO_PIN);
  Serial.print("  MOSI=");
  Serial.println(RC522_MOSI_PIN);
  Serial.print("  SS=");
  Serial.println(RC522_SS_PIN);
  Serial.print("  RST=");
  Serial.println(RC522_RST_PIN);
}

void readRfidInput() {
  if (!g_mfrc522.PICC_IsNewCardPresent() || !g_mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  const String uidHex = uidToHexString(g_mfrc522.uid);
  const unsigned long now = millis();
  if (uidHex == g_lastRfidUidHex && (now - g_lastRfidReadMs) < RFID_REPEAT_IGNORE_MS) {
    g_mfrc522.PICC_HaltA();
    g_mfrc522.PCD_StopCrypto1();
    return;
  }

  g_lastRfidUidHex = uidHex;
  g_lastRfidReadMs = now;

  Serial.print("RFID UID=");
  Serial.print(uidHex);
  Serial.print(" access_id=");
  Serial.println(uidHex);

  sendToServer(uidHex);

  g_mfrc522.PICC_HaltA();
  g_mfrc522.PCD_StopCrypto1();
}

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

String buildEventAccessId(const String &status) {
  return status + "+" + String(millis());
}

void sendEventToServer(const String &eventAccessId) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Event not sent, WiFi not connected: ");
    Serial.println(eventAccessId);
    return;
  }

  HTTPClient http;
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");
  if (g_apiKey.length() > 0) {
    http.addHeader("X-API-Key", g_apiKey);
  }

  String payload = "{\"access_id\":\"" + eventAccessId + "\"}";
  const int httpResponseCode = http.POST(payload);

  Serial.print("Event POST access_id=");
  Serial.print(eventAccessId);
  Serial.print(" HTTP=");
  Serial.println(httpResponseCode);

  http.end();
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
      sendEventToServer(buildEventAccessId("TAMPER_OPEN"));
    } else {
      Serial.println("Tamper switch restored. Stopping buzzer alarm.");
      g_tamperAlarmActive = false;
      g_tamperBuzzerState = false;
      digitalWrite(BUZZER, LOW);
      sendEventToServer(buildEventAccessId("TAMPER_RESTORED"));
    }
  }

  if (g_tamperAlarmActive && (millis() - g_lastTamperToggleMs) >= buzzerToggleIntervalMs) {
    g_tamperBuzzerState = !g_tamperBuzzerState;
    g_lastTamperToggleMs = millis();
    digitalWrite(BUZZER, g_tamperBuzzerState ? HIGH : LOW);
  }
}

void sendToServer(const String &accessId) {
  Serial.print("Processing access ID: ");
  Serial.println(accessId);
  if (WiFi.status() == WL_CONNECTED) {

    changeoverControlTo("THIS_DEVICE");

    HTTPClient http;
    http.begin(API_URL);
    http.addHeader("Content-Type", "application/json");
    if (g_apiKey.length() > 0) {
      http.addHeader("X-API-Key", g_apiKey);
    }

    String payload = "{\"access_id\":\"" + accessId + "\"}";
    feedbackProcessing();
    int httpResponseCode = http.POST(payload);

    if (httpResponseCode > 0) {
      // Server responded — use its decision, do not touch cache.
      const String response = http.getString();
      http.end();
      if (httpResponseCode == 200 && response.indexOf("\"access\": \"GRANT\"") >= 0) {
        Serial.println("Access granted by server");
        grant();
      } else {
        Serial.println("Access denied by server");
        feedbackReject();
      }
      return;
    }

    http.end();
    // POST failed (network-level error): fall through to cache below.
    Serial.print("POST failed, HTTP: ");
    Serial.println(httpResponseCode);

    // Fallback: use cached decision when server is unreachable or WiFi is down.
    auto it = g_accessMappings.find(accessId);
    if (it != g_accessMappings.end()) {
      Serial.print("Cache fallback for ID ");
      Serial.print(accessId);
      Serial.println(it->second ? ": GRANT" : ": REJECT");
      if (it->second) {
        grant();
      } else {
        feedbackReject();
      }
    } else {
      Serial.println("No cached decision for this ID. Rejecting.");
      feedbackReject();
    }

  } else {
    Serial.println("WiFi not connected.");
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
  if (MAGLOCK_PWR_GND_RALAY >= 0) {
    pinMode(MAGLOCK_PWR_GND_RALAY, OUTPUT);
  }
  pinMode(EXIT_BUTTON_INPUT_PULLUP_RELAY, OUTPUT);
  pinMode(EXIT_BUTTON_INPUT_GND_RELAY, OUTPUT);
  pinMode(RC522_RST_PIN, OUTPUT);
  pinMode(RC522_SS_PIN, OUTPUT);
  changeoverControlTo("EXTERNAL_CONTROLLER");

  attachInterrupt(digitalPinToInterrupt(WIEGAND_D0_PIN), onD0Pulse, FALLING);
  attachInterrupt(digitalPinToInterrupt(WIEGAND_D1_PIN), onD1Pulse, FALLING);

  // Initialize serial
  Serial.begin(115200);

  // Avoid WiFi driver persisting stale AP/STA configuration across reboots.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  delay(50);

  loadRuntimeConfig();

  Serial.println("=== Setup AP config at boot ===");
  Serial.print("ESP32 STA MAC: ");
  Serial.println(WiFi.macAddress());
  logStringDebug("AP SSID (macro)", String(CONFIG_AP_SSID));
  logStringDebug("AP password (loaded runtime)", g_configApPassword);
  logStringDebug("AP password (default macro)", String(CONFIG_AP_PASSWORD_DEFAULT));

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
  initRfidReader();

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
  readRfidInput();

  // Periodically refresh access mappings from server.
  if (WiFi.status() == WL_CONNECTED &&
      (g_lastMappingsFetchMs == 0 || (millis() - g_lastMappingsFetchMs) >= MAPPINGS_REFRESH_INTERVAL_MS)) {
    fetchAccessMappings();
  }

  if (CURRENT_LED_REJECTED_STATE == LOW && WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_REJECTED, HIGH);
  }

  delay(1);
}
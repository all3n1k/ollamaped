// =============================================================================
//  spider_esp.ino — Spider Robot ESP8266 Bridge Firmware
//
//  Runs on the ESP-01 module connected to the ATmega2560 via Serial2.
//  Responsibilities:
//    1. Join home WiFi (STA mode), auto-reconnect on drop
//    2. Connect to brain server TCP socket, auto-reconnect on drop
//    3. Transparent serial bridge: ATmega ↔ TCP ↔ Brain server
//    4. ArduinoOTA for wireless firmware updates (no USB needed after first flash)
//
//  Board: Generic ESP8266 Module
//  Flash: 1MB (ESP-01)
//  Arduino core: esp8266/Arduino 3.x
// =============================================================================

#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>

// -----------------------------------------------------------------------------
//  Credentials live in `secrets.h` (gitignored). Copy `secrets.example.h` to
//  `secrets.h` in this same folder and fill in real values before flashing.
// -----------------------------------------------------------------------------
#include "secrets.h"
// -----------------------------------------------------------------------------

// Serial speed must match ATmega BrainComm ESP_BAUD (Config.h)
static const uint32_t SERIAL_BAUD = 115200;

// Reconnect intervals
static const uint32_t WIFI_RETRY_MS   = 5000;
static const uint32_t TCP_RETRY_MS    = 3000;
static const uint32_t TCP_TIMEOUT_MS  = 8000;  // close dead connection after 8s silence

WiFiClient _tcp;
uint32_t _lastWifiAttempt = 0;
uint32_t _lastTcpAttempt  = 0;
uint32_t _lastRxTime      = 0;

// =============================================================================
//  setup
// =============================================================================
void setup() {
    Serial.begin(SERIAL_BAUD);

    // Use onboard LED (GPIO1=TX on ESP-01, use GPIO2 if available as LED)
    // ESP-01 has LED on GPIO2
    pinMode(2, OUTPUT);
    digitalWrite(2, HIGH);  // active low, off initially

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);  // don't wear out flash with reconnect saves

    // OTA setup — hostname visible in Arduino IDE and avahi/mDNS
    ArduinoOTA.setHostname("spider-esp");
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        _tcp.stop();  // close TCP bridge during OTA to avoid interleaving
        Serial.end();
    });
    ArduinoOTA.onEnd([]() {
        // ESP restarts automatically after OTA
    });
    ArduinoOTA.onError([](ota_error_t) {
        ESP.restart();
    });

    ArduinoOTA.begin();

    connectWifi();
    _lastRxTime = millis();
}

// =============================================================================
//  loop
// =============================================================================
void loop() {
    ArduinoOTA.handle();  // must be called every loop

    ensureWifi();
    ensureTcp();
    bridge();
    blinkStatus();
}

// =============================================================================
//  WiFi management
// =============================================================================
void connectWifi() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    _lastWifiAttempt = millis();
}

void ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    if (millis() - _lastWifiAttempt < WIFI_RETRY_MS) return;
    _lastWifiAttempt = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// =============================================================================
//  TCP management
// =============================================================================
void ensureTcp() {
    if (WiFi.status() != WL_CONNECTED) return;

    // Watchdog: close silent connection (brain server gone without TCP FIN)
    if (_tcp.connected() && millis() - _lastRxTime > TCP_TIMEOUT_MS) {
        _tcp.stop();
    }

    if (_tcp.connected()) return;
    if (millis() - _lastTcpAttempt < TCP_RETRY_MS) return;

    _lastTcpAttempt = millis();
    _tcp.connect(BRAIN_HOST, BRAIN_PORT);
    if (_tcp.connected()) {
        _lastRxTime = millis();
    }
}

// =============================================================================
//  Transparent bridge — ATmega Serial ↔ TCP
// =============================================================================
void bridge() {
    if (!_tcp.connected()) return;

    // TCP → Serial (brain → ATmega)
    while (_tcp.available()) {
        Serial.write(_tcp.read());
        _lastRxTime = millis();
    }

    // Serial → TCP (ATmega → brain)
    // Also reset watchdog here — ATmega sends state every 50ms so this
    // proves the connection is alive even when brain isn't sending commands.
    while (Serial.available()) {
        _tcp.write(Serial.read());
        _lastRxTime = millis();
    }
}

// =============================================================================
//  Status LED — blink pattern reflects connection state
//  Fast blink (200ms): no WiFi
//  Slow blink (1000ms): WiFi OK, no TCP
//  Solid on: fully connected
// =============================================================================
static uint32_t _lastBlink = 0;
static bool _ledState = false;
static uint8_t _blinkCount = 0;

void blinkStatus() {
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    bool tcpOk  = _tcp.connected();

    if (wifiOk && tcpOk) {
        digitalWrite(2, LOW);  // solid on = connected
        return;
    }

    uint16_t interval = wifiOk ? 1000 : 200;
    if (millis() - _lastBlink >= interval) {
        _lastBlink = millis();
        _ledState = !_ledState;
        digitalWrite(2, _ledState ? LOW : HIGH);  // active low
    }
}

#include "EspLink.h"
#include <avr/pgmspace.h>
#include <string.h>

#if WIFI_ENABLED

EspLink espLink;

// =============================================================================
//  Ring buffer helpers
// =============================================================================
static inline uint8_t rbNext(uint8_t i, uint8_t sz) { return (i + 1) % sz; }

void EspLink::begin() {
    Serial2.begin(ESP_BAUD);
    Serial2.setTimeout(100);
    _tcpOk       = false;
    _transparent = false;
    _lastAttempt = 0;
}

// =============================================================================
//  update() — called every loop iteration.
//  Tries to (re)connect if TCP is down. Always drains incoming data.
// =============================================================================
void EspLink::update() {
    parseIPD();

    if (_tcpOk) return;

    uint32_t now = millis();
    if (now - _lastAttempt < RETRY_MS) return;
    _lastAttempt = now;

    // If we were in transparent mode, escape back to command mode first
    if (_transparent) {
        delay(1100);                // guard time before +++
        Serial2.print(F("+++"));
        delay(1100);                // guard time after +++
        flushESP(300);
        _transparent = false;
    }

    if (wifiConnect() && tcpConnect()) {
        _tcpOk = true;
    }
}

// =============================================================================
//  write() — in transparent mode, bytes go directly over TCP
// =============================================================================
void EspLink::write(const uint8_t* data, size_t len) {
    if (!_tcpOk || !_transparent || len == 0) return;
    Serial2.write(data, len);
}

// =============================================================================
//  read() / available() — read from ring buffer populated by parseIPD()
// =============================================================================
int EspLink::read() {
    if (_rxHead == _rxTail) return -1;
    uint8_t b = _rx[_rxHead];
    _rxHead = rbNext(_rxHead, RX_BUF);
    return b;
}

int EspLink::available() const {
    return (_rxTail - _rxHead + RX_BUF) % RX_BUF;
}

// =============================================================================
//  parseIPD() — in transparent mode, raw bytes from Serial2 are TCP payload.
//  Watch for "CLOSED" (connection dropped by peer) to reset state.
// =============================================================================
void EspLink::parseIPD() {
    // In transparent mode: raw bytes arrive — no +IPD framing.
    // We still need to detect "CLOSED\r\n" from the ESP.
    // Strategy: buffer the last 8 chars and scan for "CLOSED".
    static char closedBuf[8];
    static uint8_t closedLen = 0;

    while (Serial2.available()) {
        char c = (char)Serial2.read();

        if (_transparent) {
            // Slide "CLOSED" detector
            if (closedLen < 7) {
                closedBuf[closedLen++] = c;
            } else {
                memmove(closedBuf, closedBuf + 1, 6);
                closedBuf[6] = c;
                closedLen = 7;
            }
            closedBuf[closedLen] = '\0';
            if (strstr(closedBuf, "CLOSED")) {
                _tcpOk       = false;
                _transparent = false;
                closedLen    = 0;
                return;
            }

            // Push payload byte into ring buffer
            uint8_t next = rbNext(_rxTail, RX_BUF);
            if (next != _rxHead) {
                _rx[_rxTail] = (uint8_t)c;
                _rxTail = next;
            }
        }
        // In non-transparent mode during connect sequence, atWait() reads
        // directly from Serial2 — nothing to do here.
    }
}

// =============================================================================
//  AT helpers
// =============================================================================
void EspLink::atSend(const char* cmd) {
    Serial2.println(cmd);
}

bool EspLink::atWait(const char* expect, uint16_t timeoutMs) {
    char    buf[64];
    uint8_t len   = 0;
    uint32_t start = millis();

    while (millis() - start < timeoutMs) {
        if (!Serial2.available()) continue;
        char c = (char)Serial2.read();
        if (c == '\n') {
            buf[len] = '\0';
            if (strstr(buf, expect)) return true;
            if (strstr(buf, "ERROR") || strstr(buf, "FAIL")) return false;
            len = 0;
        } else if (c != '\r' && len < sizeof(buf) - 1) {
            buf[len++] = c;
            buf[len]   = '\0';
            // Check after every char — handles ">" prompt (no newline)
            if (strstr(buf, expect)) return true;
        }
    }
    return false;
}

void EspLink::flushESP(uint16_t ms) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        while (Serial2.available()) Serial2.read();
    }
}

// =============================================================================
//  wifiConnect() — reset ESP, set STA mode, join network
// =============================================================================
bool EspLink::wifiConnect() {
    flushESP(100);

    atSend("AT+RST");
    atWait("ready", 3000);   // some firmware says "OK" then "ready"; best effort
    flushESP(300);

    atSend("AT+CWMODE_CUR=1");
    if (!atWait("OK", 2000)) return false;

    Serial2.print(F("AT+CWJAP_CUR=\""));
    Serial2.print(F(WIFI_SSID));
    Serial2.print(F("\",\""));
    Serial2.print(F(WIFI_PASSWORD));
    Serial2.println(F("\""));

    return atWait("OK", 12000);
}

// =============================================================================
//  tcpConnect() — open TCP, switch to transparent passthrough mode
// =============================================================================
bool EspLink::tcpConnect() {
    atSend("AT+CIPMUX=0");
    if (!atWait("OK", 2000)) return false;

    Serial2.print(F("AT+CIPSTART=\"TCP\",\""));
    Serial2.print(F(BRAIN_HOST));
    Serial2.print(F("\","));
    Serial2.println(BRAIN_PORT);
    if (!atWait("OK", 5000)) return false;

    // Switch to transparent passthrough — all subsequent Serial2 bytes are TCP
    atSend("AT+CIPMODE=1");
    if (!atWait("OK", 2000)) return false;

    atSend("AT+CIPSEND");          // no length — enter passthrough
    if (!atWait(">", 3000)) return false;

    _transparent = true;
    return true;
}

#endif // WIFI_ENABLED

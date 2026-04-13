#pragma once
#include <Arduino.h>
#include "Config.h"

// =============================================================================
//  EspLink — ATmega-side AT command driver for the ESP-01 on Serial2
//
//  Puts the ESP into STA mode, joins home WiFi, opens a single TCP connection
//  to the brain server, and provides send/receive that look like a Stream.
//
//  No ESP flash required — works with the factory Espressif AT firmware.
//
//  AT command flow on connect:
//    AT+RST
//    AT+CWMODE_CUR=1          (station mode)
//    AT+CWJAP_CUR="ssid","pw" (join network)
//    AT+CIPMUX=0              (single connection)
//    AT+CIPSTART="TCP","host",port
//
//  Sending:   AT+CIPSEND=N  →  wait for ">"  →  write N bytes
//  Receiving: +IPD,N:...data...  prefix stripped automatically
// =============================================================================

#if WIFI_ENABLED

class EspLink {
public:
    void begin();
    void update();          // call every loop — drains +IPD into _rxBuf
    bool connected() const  { return _tcpOk; }

    // Write bytes to brain server (max 128 per call due to AT CIPSEND limit)
    void write(const uint8_t* data, size_t len);
    void print(const char* s)  { write((const uint8_t*)s, strlen(s)); }

    // Read one byte from receive ring buffer (-1 if empty)
    int  read();
    int  available() const;

private:
    // ---- ring buffer for +IPD data stripping ----
    static const uint8_t RX_BUF = 128;
    uint8_t  _rx[RX_BUF];
    uint8_t  _rxHead = 0, _rxTail = 0;

    bool     _tcpOk       = false;
    bool     _transparent = false;   // true = in AT transparent passthrough mode
    uint32_t _lastAttempt = 0;
    static const uint32_t RETRY_MS = 8000;

    // raw AT helpers
    void     atSend(const char* cmd);
    bool     atWait(const char* expect, uint16_t timeoutMs = 5000);
    void     flushESP(uint16_t ms = 200);

    bool     wifiConnect();
    bool     tcpConnect();
    void     parseIPD();          // drain Serial2, strip +IPD headers
};

extern EspLink espLink;

#endif // WIFI_ENABLED

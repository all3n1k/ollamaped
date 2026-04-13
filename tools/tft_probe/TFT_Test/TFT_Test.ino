/**
 * TFT_Test — Quick test sketch for XW-TS0526 / UN2572 display
 * =============================================================
 * 
 * WIRING:
 *   ATmega2560          BSS138 Level Shifter         Display
 *   Pin 2 (TX)   →      HV1                   LV1   ┐
 *   Pin 3 (RX)   →      HV2                   LV2   ├─ U_TX.RX
 *   5V                   HV                          │
 *   3.3V                 LV                          │  VCC → 3.3V
 *   GND                  GND                         │  GND → GND
 *                                                   │  TFT_DET → GND
 *                                                   └──────────
 * 
 * USAGE:
 *   1. Load this sketch onto ATmega2560
 *   2. Open Serial Monitor at 115200 baud
 *   3. Send single bytes as hex (e.g., type "A0" + Enter)
 *   4. Watch the display and note which byte shows which image
 *   5. Use discovered bytes in your main application
 */

#include <Arduino.h>
#include <SoftwareSerial.h>
#include "TFT_Display.h"

#define DEBUG Serial
#define DEBUG_BAUD 115200

#define TFT_TX_PIN  2
#define TFT_RX_PIN  3

TFT_Display tft(TFT_TX_PIN, TFT_RX_PIN);

void setup() {
    DEBUG.begin(DEBUG_BAUD);
    while (!DEBUG) {}

    DEBUG.println(F("╔══════════════════════════════════════╗"));
    DEBUG.println(F("║  TFT Display Test — XW-TS0526       ║"));
    DEBUG.println(F("╚══════════════════════════════════════╝"));
    DEBUG.println(F("Type hex bytes to send (e.g., A0, 7F, 00)"));
    DEBUG.println(F("Type 'M' to run image map sweep"));
    DEBUG.println(F("Type 'R' to repeat last byte"));
    DEBUG.println();

    tft.begin(9600);
    DEBUG.println(F("TFT serial initialized at 9600 baud"));
}

uint8_t lastByte = 0x00;
bool hasLast = false;

void loop() {
    if (DEBUG.available()) {
        char c = DEBUG.read();

        if (c == 'M' || c == 'm') {
            DEBUG.println(F("\n=== Image Map Sweep ==="));
            DEBUG.println(F("Watch display. Type byte value + ENTER for each image."));
            DEBUG.println(F("Type '.' when done.\n"));
            for (uint16_t b = 0; b < 256; b++) {
                DEBUG.print(F("Byte 0x"));
                if (b < 0x10) DEBUG.print('0');
                DEBUG.print(b, HEX);
                DEBUG.print(F(": "));

                tft.writeSingleByte((uint8_t)b);
                lastByte = (uint8_t)b;
                hasLast = true;

                while (!DEBUG.available()) {}
                char resp = DEBUG.read();
                if (resp == '.') break;

                DEBUG.print(F(" → Image: "));
                DEBUG.println(resp);
            }
            DEBUG.println(F("Sweep done."));
        }
        else if (c == 'R' || c == 'r') {
            if (hasLast) {
                DEBUG.print(F("Repeating 0x"));
                DEBUG.println(lastByte, HEX);
                tft.writeSingleByte(lastByte);
            } else {
                DEBUG.println(F("No previous byte."));
            }
        }
        else if (isHexadecimalDigit(c)) {
            String input = "";
            input += c;
            while (DEBUG.available()) {
                char nc = DEBUG.read();
                if (nc == '\n' || nc == '\r') break;
                if (isHexadecimalDigit(nc)) input += nc;
            }

            if (input.length() > 0) {
                uint8_t byteVal = strtol(input.c_str(), NULL, 16);
                DEBUG.print(F("Sending 0x"));
                DEBUG.println(byteVal, HEX);

                tft.writeSingleByte(byteVal);
                lastByte = byteVal;
                hasLast = true;

                delay(50);
                uint8_t respBuf[64];
                if (tft.hasResponse(respBuf, sizeof(respBuf), 100)) {
                    DEBUG.print(F("Response: "));
                    for (uint8_t i = 0; i < tft.lastResponseLen; i++) {
                        DEBUG.print(respBuf[i], HEX);
                        DEBUG.print(' ');
                    }
                    DEBUG.println();
                }
            }
        }
    }

    uint8_t respBuf[64];
    if (tft.hasResponse(respBuf, sizeof(respBuf), 10)) {
        DEBUG.print(F("Async response: "));
        for (uint8_t i = 0; i < tft.lastResponseLen; i++) {
            DEBUG.print(tft.lastResponse[i], HEX);
            DEBUG.print(' ');
        }
        DEBUG.println();
    }
}

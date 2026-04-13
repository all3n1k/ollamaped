/**
 * TFT Power Controller - XW-TS0526 / UN2572
 * ============================================
 * Provides stable 3.3V power to the display and allows
 * manual control of the UART line for protocol discovery.
 * 
 * WIRING:
 *   Arduino 5V  --> BSS138 HV (level shifter reference)
 *   Arduino GND --> BSS138 GND + Display GND
 *   Arduino 3.3V --> Display VCC (via BSS138 LV or direct)
 *   
 *   For UART control (half-duplex):
 *   Arduino Pin 2 (TX) --> BSS138 HV1 --> BSS138 LV1 --> Display U_TX.RX
 *   
 *   TFT_DET is handled by this sketch (GND)
 * 
 * CONTROL:
 *   Open Serial Monitor at 115200 baud
 *   Type hex bytes to send (e.g., "A0", "7F")
 *   Type "ON" to keep display powered
 *   Type "OFF" to cut power
 */

#include <Arduino.h>

#define POWER_PIN    4    // Controls display power (high = on)
#define TFT_DET_PIN  5    // TFT_DET control (low = active)
#define UART_TX_PIN  2    // SoftwareSerial TX to display

#define DEBUG Serial
#define DEBUG_BAUD 115200

void setup() {
    DEBUG.begin(DEBUG_BAUD);
    while (!DEBUG) {}
    
    // Power control
    pinMode(POWER_PIN, OUTPUT);
    digitalWrite(POWER_PIN, HIGH);  // Power on
    
    // TFT_DET to GND (active low on display)
    pinMode(TFT_DET_PIN, OUTPUT);
    digitalWrite(TFT_DET_PIN, LOW);  // Active
    
    // Initialize display power
    delay(500);
    
    DEBUG.println(F("TFT Power Controller"));
    DEBUG.println(F("========================"));
    DEBUG.println(F("Pin 4: Display power (HIGH=on)"));
    DEBUG.println(F("Pin 5: TFT_DET (LOW=active)"));
    DEBUG.println(F("Pin 2: UART TX to display"));
    DEBUG.println();
    DEBUG.println(F("Commands:"));
    DEBUG.println(F("  [hex]   - Send byte (e.g., A0, 7F)"));
    DEBUG.println(F("  ON      - Power on display"));
    DEBUG.println(F("  OFF     - Power off display"));
    DEBUG.println(F("  DET     - Toggle TFT_DET"));
    DEBUG.println(F("  SWEEP   - Auto sweep 0x00-0xFF"));
    DEBUG.println(F("  HOLD    - Keep sending last byte"));
    DEBUG.println();
    DEBUG.println(F("Display power: ON"));
    DEBUG.println(F("TFT_DET: Active (LOW)"));
}

void loop() {
    if (DEBUG.available()) {
        String cmd = DEBUG.readStringUntil('\n');
        cmd.trim();
        cmd.toUpperCase();
        
        if (cmd == "ON") {
            digitalWrite(POWER_PIN, HIGH);
            DEBUG.println("Power: ON");
            delay(100);
        }
        else if (cmd == "OFF") {
            digitalWrite(POWER_PIN, LOW);
            DEBUG.println("Power: OFF");
        }
        else if (cmd == "DET") {
            static bool det_state = false;
            det_state = !det_state;
            digitalWrite(TFT_DET_PIN, det_state ? HIGH : LOW);
            DEBUG.print("TFT_DET: ");
            DEBUG.println(det_state ? "HIGH" : "LOW");
        }
        else if (cmd == "SWEEP") {
            DEBUG.println("Starting sweep 0x00-0xFF...");
            for (int b = 0; b < 256; b++) {
                DEBUG.print(".");
                DEBUG.print(b, HEX);
                DEBUG.print(" ");
                DEBUG.flush();
                
                // Send byte manually (no SoftwareSerial needed for TX-only)
                pinMode(UART_TX_PIN, OUTPUT);
                digitalWrite(UART_TX_PIN, LOW);
                
                // Start bit
                delayMicroseconds(104);  // 9600 baud = ~104us per bit
                
                // Send 8 bits, LSB first
                for (int bit = 0; bit < 8; bit++) {
                    digitalWrite(UART_TX_PIN, (b >> bit) & 1 ? HIGH : LOW);
                    delayMicroseconds(104);
                }
                
                // Stop bit
                digitalWrite(UART_TX_PIN, HIGH);
                delayMicroseconds(104);
                
                // Small delay between bytes
                delay(50);
                
                // Check for serial input (to abort)
                if (DEBUG.available()) {
                    DEBUG.println();
                    DEBUG.println("Sweep aborted.");
                    break;
                }
            }
            DEBUG.println();
            DEBUG.println("Sweep complete.");
        }
        else if (cmd == "HOLD") {
            DEBUG.println("Hold mode - send a byte first, then type HOLD");
        }
        else {
            // Try to parse as hex byte
            char *end;
            unsigned long val = strtoul(cmd.c_str(), &end, 16);
            if (val <= 255 && end != cmd.c_str()) {
                uint8_t b = (uint8_t)val;
                
                DEBUG.print("Sending: 0x");
                DEBUG.println(b, HEX);
                
                // Manual UART send
                pinMode(UART_TX_PIN, OUTPUT);
                digitalWrite(UART_TX_PIN, LOW);
                delayMicroseconds(104);
                
                for (int bit = 0; bit < 8; bit++) {
                    digitalWrite(UART_TX_PIN, (b >> bit) & 1 ? HIGH : LOW);
                    delayMicroseconds(104);
                }
                
                digitalWrite(UART_TX_PIN, HIGH);
                delayMicroseconds(104);
                
                DEBUG.println("Sent.");
            }
            else {
                DEBUG.println("Unknown command.");
            }
        }
    }
}

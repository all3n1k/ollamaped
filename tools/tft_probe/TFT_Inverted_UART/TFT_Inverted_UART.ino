/**
 * TFT UART Bridge (Normal - Non-Inverted)
 * ======================================
 * Receives UART from PC, forwards to display at same baud rate.
 * 
 * WIRING:
 *   PC USB-UART RX --> Arduino Pin 0 (RX)  
 *   Arduino Pin 1 (TX) --> Level Shifter --> Display U_TX.RX
 *   
 *   GND --> Display GND
 *   
 * USE SERIAL MONITOR AT 9600 BAUD
 */

#define BAUD 9600

void setup() {
    Serial.begin(BAUD);
    Serial.println("TFT UART Bridge Ready");
    Serial.println("Type hex bytes or text to send");
}

void loop() {
    // Forward all data from PC to display
    if (Serial.available()) {
        Serial.write(Serial.read());
    }
}

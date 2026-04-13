// =============================================================================
//  esp_passthrough.ino — ATmega2560 → ESP8266 serial passthrough
//
//  PURPOSE: One-time use only. Lets you flash the ESP-01 module through the
//  ATmega's USB connection. After flashing spider_esp.ino to the ESP, reflash
//  spider_firmware.ino to the ATmega and discard this sketch.
//
//  PROCEDURE:
//    1. Flash this sketch to ATmega2560 via USB (normal arduino-cli upload)
//    2. Bridge GPIO0 (pin 3 of ESP-01 connector) to GND (pin 1) with a dupont wire
//    3. Power cycle the board (unplug/replug USB) — ESP enters bootloader mode
//    4. Run: esptool.py --port COM3 --baud 115200 write_flash 0x00000 spider_esp.bin
//    5. Remove the GPIO0-GND jumper wire
//    6. Power cycle — ESP boots the new firmware
//    7. Flash spider_firmware.ino back to the ATmega
// =============================================================================

void setup() {
    Serial.begin(115200);   // USB ↔ PC
    Serial2.begin(115200);  // ATmega ↔ ESP-01 (matches ESP default baud)

    // Signal that passthrough is active (pin 13 LED on)
    pinMode(13, OUTPUT);
    digitalWrite(13, HIGH);
}

void loop() {
    if (Serial.available())  Serial2.write(Serial.read());
    if (Serial2.available()) Serial.write(Serial2.read());
}

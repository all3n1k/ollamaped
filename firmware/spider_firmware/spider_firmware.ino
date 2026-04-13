/*
 * Spider Robot Firmware
 * ATmega2560 — Brain-controlled quadruped pet
 *
 * Architecture:
 *   FNQRBasic  — Servo IK engine (original, unmodified)
 *   Behavior   — Spider pet state machine & animations
 *   BrainComm  — JSON protocol over Serial (USB) and Serial2 (ESP8266)
 *   Config.h   — All pins, tuning, DEV_MODE flag
 */

// Increase hardware serial RX buffer before any includes.
// Default is 64 bytes — too small when loop() blocks during gait steps.
// 256 bytes gives ~22ms headroom at 115200 baud before overflow.
#define SERIAL_RX_BUFFER_SIZE 256

#ifndef ARDUINO_AVR_MEGA2560
#error Wrong board. Select "Arduino/Genuino Mega or Mega 2560".
#endif

#include "Config.h"
#include "FNQRBasic.h"
#include "Behavior.h"
#include "BrainComm.h"
#include "Buzzer.h"
#include <FlexiTimer2.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
RobotAction robotAction;
Behavior    behavior(robotAction);
BrainComm   brainComm(behavior);

// ---------------------------------------------------------------------------
// FlexiTimer2 ISR — drives all servo interpolation at 20ms
// ---------------------------------------------------------------------------
void servoUpdateISR() {
    robotAction.robot.Update();
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    // Servo update interrupt — 20ms matches original firmware timing
    FlexiTimer2::set(SERVO_UPDATE_MS, servoUpdateISR);
    FlexiTimer2::start();

#ifdef DEV_MODE
    // Bypass battery voltage check: force all power groups ON
    // Safe with USB only — servos won't move without batteries,
    // but firmware will run and respond to commands.
    pinMode(POWER_GROUP_1_PIN, OUTPUT);
    pinMode(POWER_GROUP_2_PIN, OUTPUT);
    pinMode(POWER_GROUP_3_PIN, OUTPUT);
    digitalWrite(POWER_GROUP_1_PIN, HIGH);
    digitalWrite(POWER_GROUP_2_PIN, HIGH);
    digitalWrite(POWER_GROUP_3_PIN, HIGH);

    // Set power object to manual mode (no auto voltage switching)
    // adcReference=5.0, proportion=0.5 (10k/10k divider on V2 boards)
    robotAction.robot.power.Set(5.0f, 0.5f, false);
#else
    // Production: auto power group management with battery voltage sensing
    // The correct ADC calibration is derived at runtime in the original firmware;
    // we replicate the V3 path here using the board's internal 2.5V reference.
    float defaultRef  = 2.5f / analogRead(A6) * 1023.0f;
    float externalRef = defaultRef * 2.0f / (6.2f + 2.0f) / analogRead(A8) * 1023.0f;
    robotAction.robot.power.Set(externalRef, 2.0f / (6.2f + 2.0f), true);
#endif

    brainComm.begin();
    behavior.begin();
    buzzer.begin();
    buzzer.play(BuzzPattern::BOOT);   // startup chime
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void loop() {
    // Read commands and send state reports
    brainComm.update();

    // Drive behavior animations
    behavior.update();

    // Advance buzzer pattern sequencer
    buzzer.update();

#ifndef DEV_MODE
    // Update power groups in production mode
    robotAction.robot.power.Update();
#endif
}

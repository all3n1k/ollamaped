#pragma once

// =============================================================================
//  Spider Robot — Configuration
//  All pins, tuning constants, and build flags live here.
// =============================================================================

// --- Build flags -------------------------------------------------------------

// DEV_MODE: bypass battery voltage check, force servo power groups ON.
// Safe to use with USB only — servos just won't move without batteries.
// Comment out for autonomous/production use.
#define DEV_MODE

// --- Serial ------------------------------------------------------------------
#define SERIAL_BAUD       115200
#define ESP_BAUD          115200   // ESP8266 on Serial2

// --- WiFi (AT firmware mode — no ESP flash required) -------------------------
// Set WIFI_ENABLED to 1 to have the ATmega configure the ESP-01 via AT commands
// and connect to your home network + brain server over TCP.
// Leave 0 to use USB serial only.
#define WIFI_ENABLED       0
// WIFI_SSID, WIFI_PASSWORD, BRAIN_HOST, BRAIN_PORT are defined in Secrets.h
// (gitignored). Copy Secrets.example.h to Secrets.h and fill in your values.
#include "Secrets.h"

// --- Power group pins (MOSFETs gating battery to servos) --------------------
#define POWER_GROUP_1_PIN  A15
#define POWER_GROUP_2_PIN  A13
#define POWER_GROUP_3_PIN  A14

// Battery voltage thresholds
#define VOLTAGE_ON         6.5f
#define VOLTAGE_WARN       7.0f   // sends {"t":"evt","e":"low_bat"} to brain once
#define VOLTAGE_OFF        5.5f

// Voltage sensing: pin A7, ADC divider 2/(6.2+2)
#define VOLTAGE_SENSE_PIN  A7

// --- Servo pins --------------------------------------------------------------
// Leg numbering: 1=front-left, 2=back-left, 3=front-right, 4=back-right
// Joint A=hip, B=shoulder, C=knee
#define LEG1_A  22
#define LEG1_B  23
#define LEG1_C  24

#define LEG2_A  25
#define LEG2_B  26
#define LEG2_C  27

#define LEG3_A  39
#define LEG3_B  38
#define LEG3_C  37

#define LEG4_A  36
#define LEG4_B  35
#define LEG4_C  34

// --- EEPROM calibration addresses -------------------------------------------
// Offsets for each joint (12 joints x 1 byte each, starting at 100)
#define EEPROM_OFFSET_BASE   100
#define EEPROM_STATE_ADDR    140

// --- Motion tuning -----------------------------------------------------------
#define SERVO_UPDATE_MS      20     // FlexiTimer2 interrupt period
#define LEG_LIFT_HEIGHT      25.0f  // mm, how high legs lift during walk
#define BODY_HEIGHT_DEFAULT  15.0f  // mm, default body height above ground
#define STEP_DISTANCE        2.0f   // mm, step resolution

// Trot gait
#define TROT_HALF_STRIDE     15.0f  // mm, ±15 from neutral = 30mm total stride
#define TROT_LIFT_HEIGHT     25.0f  // mm, same as crawl lift
#define TROT_SPEED           10.0f  // mm/tick, faster than crawl (7.5)

// --- Command protocol --------------------------------------------------------
#define CMD_BUFFER_SIZE      128    // max incoming JSON command length
#define STATE_REPORT_MS      50     // how often to send state back to brain

// --- Status LED --------------------------------------------------------------
#define STATUS_LED_PIN       13

// --- Buzzer ------------------------------------------------------------------
// PC case speaker wired to P3 pin A0 (PF0).
// Timer4 CTC mode — no conflict with FlexiTimer2 (Timer2) or Servo lib (Timer1).
#define BUZZER_PIN           A0

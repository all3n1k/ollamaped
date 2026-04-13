#pragma once
#include <Arduino.h>
#include "Config.h"

// =============================================================================
//  Buzzer — non-blocking software tone generator, no hardware timer ISR.
//
//  All hardware timers are claimed at compile time by Servo (T1/T3/T4/T5) and
//  FlexiTimer2 (T2). Instead, Buzzer::update() polls micros() to toggle the
//  pin at the correct half-period. Must be called from both loop() and the
//  brainCommYield() callback so tones continue during gait blocking waits.
//
//  Pin: A0 (PF0) — see Config.h BUZZER_PIN.
// =============================================================================

enum class BuzzPattern : uint8_t {
    BOOT    = 0,   // rising 3-note startup sweep
    HAPPY   = 1,   // quick double-chirp up
    EXCITED = 2,   // rapid staccato burst
    CURIOUS = 3,   // gentle rising tone
    ALERT   = 4,   // sharp double pulse
    SLEEPY  = 5,   // slow descending sigh
    SHY     = 6,   // soft low descend
    GREET   = 7,   // cheerful two-note hello
    ACK     = 8,   // single short blip
    CONFUSED = 9,   // rising and falling '?'
    SAD     = 10,   // slow descending slide
    VICTORY  = 11,   // triumphant fanfare
    WARNING  = 12,   // sharp repetitive alarm
};

class Buzzer {
public:
    void begin();
    void update();                        // call from loop() AND yield callback

    void tone(uint16_t hz, uint16_t ms);  // play single tone, non-blocking
    void stop();
    void play(BuzzPattern p);             // play named emotional pattern

    bool isBusy() const { return _busy; }

private:
    bool     _busy         = false;
    bool     _toneActive   = false;
    uint16_t _hz           = 0;
    uint32_t _halfPeriodUs = 0;
    uint32_t _lastToggle   = 0;
    uint32_t _stepEndAt    = 0;

    uint8_t         _patStep = 0;
    const uint16_t* _patPtr  = nullptr;   // PROGMEM pointer

    void _startTone(uint16_t hz);
    void _stopTone();
    void _nextStep();
};

extern Buzzer buzzer;

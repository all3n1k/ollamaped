#include "Buzzer.h"
#include <avr/pgmspace.h>

// =============================================================================
//  Pattern table — pairs of (freq_hz, duration_ms), PROGMEM.
//  REST = freq 1 (inaudible, pin stays low).  END = (0, 0).
// =============================================================================

#define NOTE(f,d) (f),(d)
#define REST(d)   NOTE(1,d)
#define END       NOTE(0,0)

static const uint16_t PROGMEM _pat_boot[]    = { NOTE(440,80), REST(25), NOTE(554,80), REST(25), NOTE(659,130), END };
static const uint16_t PROGMEM _pat_happy[]   = { NOTE(784,55), REST(18), NOTE(988,75), END };
static const uint16_t PROGMEM _pat_excited[] = { NOTE(880,35), NOTE(1047,35), NOTE(880,35), NOTE(1047,35), NOTE(1319,75), END };
static const uint16_t PROGMEM _pat_curious[] = { NOTE(494,55), REST(18), NOTE(587,95), END };
static const uint16_t PROGMEM _pat_alert[]   = { NOTE(1047,35), REST(12), NOTE(1047,35), END };
static const uint16_t PROGMEM _pat_sleepy[]  = { NOTE(392,110), REST(35), NOTE(294,150), END };
static const uint16_t PROGMEM _pat_shy[]     = { NOTE(330,75), NOTE(262,115), END };
static const uint16_t PROGMEM _pat_greet[]   = { NOTE(659,75), REST(18), NOTE(784,55), REST(18), NOTE(659,55), END };
static const uint16_t PROGMEM _pat_ack[]     = { NOTE(1047,28), END };
static const uint16_t PROGMEM _pat_confused[]   = { NOTE(440,100), NOTE(554,100), NOTE(440,100), NOTE(330,200), END };
static const uint16_t PROGMEM _pat_sad[]       = { NOTE(330,200), NOTE(262,400), END };
static const uint16_t PROGMEM _pat_victory[]   = { NOTE(523,100), NOTE(659,100), NOTE(784,100), NOTE(1047,300), END };
static const uint16_t PROGMEM _pat_warning[]   = { NOTE(880,100), REST(50), NOTE(880,100), REST(50), NOTE(880,100), END };

static const uint16_t* const _patterns[] PROGMEM = {
    _pat_boot, _pat_happy, _pat_excited, _pat_curious,
    _pat_alert, _pat_sleepy, _pat_shy, _pat_greet, _pat_ack,
    _pat_confused, _pat_sad, _pat_victory, _pat_warning,
};

// =============================================================================
//  Implementation
// =============================================================================

Buzzer buzzer;

void Buzzer::begin() {
    DDRF  |=  (1 << PF0);    // A0 as output
    PORTF &= ~(1 << PF0);    // start low
}

void Buzzer::_startTone(uint16_t hz) {
    _hz = hz;
    if (hz > 1) {
        _halfPeriodUs = 500000UL / hz;   // 1,000,000 / (2*hz)
        _lastToggle   = micros();
        _toneActive   = true;
    } else {
        _toneActive   = false;
        PORTF &= ~(1 << PF0);
    }
}

void Buzzer::_stopTone() {
    _toneActive = false;
    PORTF &= ~(1 << PF0);
}

void Buzzer::tone(uint16_t hz, uint16_t ms) {
    _patPtr   = nullptr;
    _patStep  = 0;
    _busy     = true;
    _stepEndAt = millis() + ms;
    _startTone(hz);
}

void Buzzer::stop() {
    _stopTone();
    _patPtr  = nullptr;
    _patStep = 0;
    _busy    = false;
}

void Buzzer::play(BuzzPattern p) {
    _patPtr  = (const uint16_t*)pgm_read_ptr(&_patterns[(uint8_t)p]);
    _patStep = 0;
    _busy    = true;
    _nextStep();
}

void Buzzer::_nextStep() {
    if (!_patPtr) return;
    uint16_t hz = pgm_read_word(_patPtr + _patStep);
    uint16_t ms = pgm_read_word(_patPtr + _patStep + 1);
    _patStep += 2;
    if (hz == 0 && ms == 0) { stop(); return; }
    _stepEndAt = millis() + ms;
    _startTone(hz);
}

// Called from loop() AND brainCommYield() — must stay fast (<5µs typical).
void Buzzer::update() {
    if (!_busy) return;

    // Toggle pin to generate the square wave
    if (_toneActive) {
        uint32_t now = micros();
        if (now - _lastToggle >= _halfPeriodUs) {
            _lastToggle += _halfPeriodUs;
            PINF = (1 << PF0);              // write 1 to PIN reg = toggle output
        }
    }

    // Advance to next pattern step when current step duration expires
    if ((int32_t)(millis() - _stepEndAt) >= 0) {
        if (_patPtr) _nextStep();
        else         stop();
    }
}

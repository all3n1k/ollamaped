#include "Behavior.h"
#include <avr/pgmspace.h>

// =============================================================================
//  64-entry sine table in PROGMEM — covers 0..2π in 64 steps.
//  Values scaled to int8_t range (-127..127).
//  Usage: fastSin(phase) where phase is uint8_t 0-255.
//  phase >> 2 maps 256 steps → 64 table entries.
// =============================================================================
static const int8_t PROGMEM sinTable[64] = {
     0,  12,  25,  37,  49,  60,  71,  81,
    90,  98, 106, 113, 118, 123, 126, 127,
   127, 126, 123, 118, 113, 106,  98,  90,
    81,  71,  60,  49,  37,  25,  12,   0,
   -12, -25, -37, -49, -60, -71, -81, -90,
   -98,-106,-113,-118,-123,-126,-127,-127,
  -126,-123,-118,-113,-106, -98, -90, -81,
   -71, -60, -49, -37, -25, -12,   0,  12
};

// phase 0-255 → sine value -1.0 .. +1.0
static inline float fastSin(uint8_t phase) {
    return (float)(int8_t)pgm_read_byte(&sinTable[phase >> 2]) * (1.0f / 127.0f);
}

// phase 0-255, scale in mm → result in mm
static inline float fsin(uint8_t phase, float scale) {
    return fastSin(phase) * scale;
}

// =============================================================================
//  behaviorName — strings in PROGMEM, copied to a shared buffer on read.
//  Saves ~80 bytes SRAM vs keeping them in data segment.
// =============================================================================
static char _nameBuf[10];   // longest name is "calibrate" (9 chars + null)

const char* behaviorName(BehaviorState s) {
    static const char n_idle[]    PROGMEM = "idle";
    static const char n_alert[]   PROGMEM = "alert";
    static const char n_happy[]   PROGMEM = "happy";
    static const char n_curious[] PROGMEM = "curious";
    static const char n_sleepy[]  PROGMEM = "sleepy";
    static const char n_walk_f[]  PROGMEM = "walk_f";
    static const char n_walk_b[]  PROGMEM = "walk_b";
    static const char n_turn_l[]  PROGMEM = "turn_l";
    static const char n_turn_r[]  PROGMEM = "turn_r";
    static const char n_sleep[]   PROGMEM = "sleep";
    static const char n_trot_f[]  PROGMEM = "trot_f";
    static const char n_trot_b[]  PROGMEM = "trot_b";
    static const char n_excited[] PROGMEM = "excited";
    static const char n_calib[]   PROGMEM = "calibrate";
    static const char n_unk[]     PROGMEM = "unknown";

    const char* p;
    switch (s) {
        case BehaviorState::IDLE:      p = n_idle;    break;
        case BehaviorState::ALERT:     p = n_alert;   break;
        case BehaviorState::HAPPY:     p = n_happy;   break;
        case BehaviorState::CURIOUS:   p = n_curious; break;
        case BehaviorState::SLEEPY:    p = n_sleepy;  break;
        case BehaviorState::WALKING_F: p = n_walk_f;  break;
        case BehaviorState::WALKING_B: p = n_walk_b;  break;
        case BehaviorState::TURNING_L: p = n_turn_l;  break;
        case BehaviorState::TURNING_R: p = n_turn_r;  break;
        case BehaviorState::SLEEP:     p = n_sleep;   break;
        case BehaviorState::TROT_F:    p = n_trot_f;   break;
        case BehaviorState::TROT_B:    p = n_trot_b;   break;
        case BehaviorState::EXCITED:   p = n_excited;  break;
        case BehaviorState::CALIBRATE: p = n_calib;    break;
        default:                       p = n_unk;      break;
    }
    strncpy_PF(_nameBuf, (uint_farptr_t)p, sizeof(_nameBuf) - 1);
    _nameBuf[sizeof(_nameBuf) - 1] = '\0';
    return _nameBuf;
}

// =============================================================================
//  Lifecycle
// =============================================================================

void Behavior::begin() {
    _action.Start();
    _state         = BehaviorState::IDLE;
    _pending       = BehaviorState::IDLE;
    _stateEnteredAt = millis();
    _lastTickAt    = 0;
    _phase         = 0;
    _phaseStep     = PHASE_STEP_NORMAL;
    _trotPhase     = false;
    _action.ActiveMode();
}

void Behavior::setState(BehaviorState next) {
    if (next == _state && next == _pending) return;
    _pending = next;
}

bool Behavior::isBusy() const {
    return _action.robot.leg1.isBusy |
           _action.robot.leg2.isBusy |
           _action.robot.leg3.isBusy |
           _action.robot.leg4.isBusy;
}

uint32_t Behavior::elapsed() const {
    return millis() - _stateEnteredAt;
}

// =============================================================================
//  Main tick — rate-limited to SERVO_UPDATE_MS (20ms).
//  Aligns behavior animation with the servo ISR cadence.
// =============================================================================
void Behavior::update() {
    uint32_t now = millis();
    if (now - _lastTickAt < SERVO_UPDATE_MS) return;
    _lastTickAt = now;

    // Apply pending state transition when legs are free
    if (_pending != _state && !isBusy()) {
        onEnter(_pending);
        _state          = _pending;
        _stateEnteredAt = now;
        _phase          = 0;
    }

    // Advance phase — uint8_t wraps at 256 automatically, no branch needed.
    // Default 6 steps/tick @ 20ms = full cycle in ~43 ticks = ~860ms
    _phase += _phaseStep;

#ifndef DEV_MODE
    // Battery protection: auto-sleep below cutoff voltage
    if (_state != BehaviorState::SLEEP &&
        _action.robot.power.voltage > 0.5f &&
        _action.robot.power.voltage < VOLTAGE_OFF) {
        _pending = BehaviorState::SLEEP;
    }
#endif

    switch (_state) {
        case BehaviorState::IDLE:      tickIdle();        break;
        case BehaviorState::ALERT:     tickAlert();       break;
        case BehaviorState::HAPPY:     tickHappy();       break;
        case BehaviorState::CURIOUS:   tickCurious();     break;
        case BehaviorState::SLEEPY:    tickSleepy();      break;
        case BehaviorState::SLEEP:                        break;
        case BehaviorState::WALKING_F: tickWalkForward();  break;
        case BehaviorState::WALKING_B: tickWalkBackward(); break;
        case BehaviorState::TURNING_L: tickTurnLeft();     break;
        case BehaviorState::TURNING_R: tickTurnRight();    break;
        case BehaviorState::TROT_F:    tickTrotForward();  break;
        case BehaviorState::TROT_B:    tickTrotBackward(); break;
        case BehaviorState::EXCITED:   tickExcited();      break;
        case BehaviorState::CALIBRATE:                     break;  // no animation — held by user nudges
    }
}

// =============================================================================
//  State entry — set posture on transition
// =============================================================================
void Behavior::onEnter(BehaviorState s) {
    switch (s) {
        case BehaviorState::IDLE:
        case BehaviorState::SLEEPY:
        case BehaviorState::HAPPY:
        case BehaviorState::CURIOUS:
            _action.InitialState();
            _action.ActiveMode();
            break;

        case BehaviorState::ALERT:
            _action.InitialState();
            _action.ActiveMode();
            _action.MoveBody(0, 0, 10);
            break;

        case BehaviorState::SLEEP:
            _action.SleepMode();
            break;

        case BehaviorState::WALKING_F:
        case BehaviorState::WALKING_B:
        case BehaviorState::TURNING_L:
        case BehaviorState::TURNING_R:
            _action.ActiveMode();
            break;

        case BehaviorState::TROT_F:
        case BehaviorState::TROT_B:
            _action.InitialState();
            _action.ActiveMode();
            _trotPhase = false;
            break;

        case BehaviorState::EXCITED:
            _action.InitialState();
            _action.ActiveMode();
            break;

        case BehaviorState::CALIBRATE:
            // Belly-down pose with all 4 legs raised above the printed
            // calibration graph (z=+35mm). Disables servo offsets so the
            // user nudges the raw IK position into alignment with the marks.
            _action.robot.CalibrateState();
            break;
    }
}

// EXCITED — large fast waggle + double-frequency bounce.
// Uses _phase * 3 locally (triple speed) without touching _phaseStep,
// so SPEED command and other states are unaffected.
void Behavior::tickExcited() {
    if (isBusy()) return;
    uint8_t fp = (uint8_t)(_phase * 3);        // local 3× speed
    float w = fsin(fp, 14.0f);                 // ±14mm waggle (vs happy ±8mm)
    float b = fabsf(fsin((uint8_t)(fp << 1), 7.0f)); // ±7mm bounce, double freq
    moveBodyDZ(w, 0, b);
}

// =============================================================================
//  MoveBody with dead-zone — skip if delta < 0.5mm on all axes.
//  Prevents flooding the IK engine with redundant position updates.
// =============================================================================
void Behavior::moveBodyDZ(float x, float y, float z) {
    if (fabsf(x - _lastX) < 0.5f &&
        fabsf(y - _lastY) < 0.5f &&
        fabsf(z - _lastZ) < 0.5f) return;
    _lastX = x; _lastY = y; _lastZ = z;
    _action.MoveBody(x, y, z);
}

// =============================================================================
//  Animation ticks
// =============================================================================

// IDLE — breathing (±3mm Z) + slow sway (±2mm X), half-speed phase for sway
void Behavior::tickIdle() {
    if (isBusy()) return;
    moveBodyDZ(fsin(_phase >> 1, 2.0f),   // sway: half speed
               0,
               fsin(_phase,    3.0f));     // breath: normal speed
}

// ALERT — body raised +10mm, slow lateral scan
void Behavior::tickAlert() {
    if (isBusy()) return;
    // Phase step /4 for very slow scan
    moveBodyDZ(fsin(_phase >> 2, 4.0f), 2.0f, 10.0f);
}

// HAPPY — fast waggle + bounce (double phase speed)
void Behavior::tickHappy() {
    if (isBusy()) return;
    uint8_t fast = _phase << 1;   // double speed, wraps naturally
    float w = fsin(fast, 8.0f);
    float b = fabsf(fsin(fast, 4.0f));
    moveBodyDZ(w, 0, b);
}

// CURIOUS — gentle forward lean + slow head tilt
void Behavior::tickCurious() {
    if (isBusy()) return;
    moveBodyDZ(fsin(_phase >> 2, 5.0f), 5.0f, 3.0f);
}

// SLEEPY — very slow droop, auto-transitions to SLEEP after 10s
void Behavior::tickSleepy() {
    if (isBusy()) return;
    float droop = fsin(_phase >> 3, 2.0f);               // very slow oscillation
    float sag   = -min(elapsed() / 8000.0f, 1.0f) * 5.0f; // sink over 8s
    moveBodyDZ(0, 0, droop + sag);
    if (elapsed() > 10000) setState(BehaviorState::SLEEP);
}

// =============================================================================
//  Gait ticks — yield to comm while waiting for ISR to finish moving legs
// =============================================================================

void Behavior::waitFreeYielding() {
    while (isBusy()) yieldComm();
}

void Behavior::tickWalkForward() {
    waitFreeYielding();
    if (_pending != _state) return;
    _action.CrawlForward();
    waitFreeYielding();
}

void Behavior::tickWalkBackward() {
    waitFreeYielding();
    if (_pending != _state) return;
    _action.CrawlBackward();
    waitFreeYielding();
}

void Behavior::tickTurnLeft() {
    waitFreeYielding();
    if (_pending != _state) return;
    _action.TurnLeft();
    waitFreeYielding();
}

void Behavior::tickTurnRight() {
    waitFreeYielding();
    if (_pending != _state) return;
    _action.TurnRight();
    waitFreeYielding();
}

// =============================================================================
//  Trot gait — diagonal pairs (leg1+leg4, leg2+leg3) swing in alternation.
//
//  Boot/neutral stance:
//    leg1 (front-left):  (-90, +90, 0)   leg3 (front-right): (+90, +90, 0)
//    leg2 (back-left):   (-90, -90, 0)   leg4 (back-right):  (+90, -90, 0)
//
//  Diagonal pairs:
//    Pair A = leg1 + leg4  (front-left + back-right)
//    Pair B = leg2 + leg3  (back-left  + front-right)
//
//  Phase A targets  (Pair A extended forward, Pair B retracted):
//    leg1: (-90, 90+S, 0)   leg2: (-90, -90-S, 0)
//    leg3: (+90, 90-S, 0)   leg4: (+90, -90+S, 0)
//
//  Phase B targets  (Pair B extended forward, Pair A retracted):
//    leg1: (-90, 90-S, 0)   leg2: (-90, -90+S, 0)
//    leg3: (+90, 90+S, 0)   leg4: (+90, -90-S, 0)
//
//  Each phase: swing legs lift to TROT_LIFT_HEIGHT, all 4 legs move to next
//  target simultaneously, then lifted pair plants (Z→0).
//  robot.SetSpeed() uses TROT_SPEED (faster than crawl default of 7.5).
// =============================================================================

static const float _TS = TROT_HALF_STRIDE;  // half-stride
static const float _TL = TROT_LIFT_HEIGHT;  // lift height

void Behavior::tickTrotForward() {
    waitFreeYielding();
    if (_pending != _state) return;

    _action.robot.SetSpeed(TROT_SPEED);

    RobotLegsPoints swing, land;

    if (!_trotPhase) {
        // Pair A swings to extended, Pair B slides to retracted
        swing = RobotLegsPoints(
            Point(-90.0f, 90.0f + _TS, _TL),   // leg1: lift + step forward
            Point(-90.0f, -90.0f - _TS, 0.0f), // leg2: slide back (planted)
            Point( 90.0f, 90.0f - _TS, 0.0f),  // leg3: slide back (planted)
            Point( 90.0f, -90.0f + _TS, _TL)   // leg4: lift + step forward
        );
        land = swing;
        land.leg1.z = 0.0f;
        land.leg4.z = 0.0f;
    } else {
        // Pair B swings to extended, Pair A slides to retracted
        swing = RobotLegsPoints(
            Point(-90.0f, 90.0f - _TS, 0.0f),  // leg1: slide back (planted)
            Point(-90.0f, -90.0f + _TS, _TL),  // leg2: lift + step forward
            Point( 90.0f, 90.0f + _TS, _TL),   // leg3: lift + step forward
            Point( 90.0f, -90.0f - _TS, 0.0f)  // leg4: slide back (planted)
        );
        land = swing;
        land.leg2.z = 0.0f;
        land.leg3.z = 0.0f;
    }

    _action.robot.MoveTo(swing);
    waitFreeYielding();
    if (_pending != _state) { _action.robot.SetSpeed(7.5f); return; }

    _action.robot.MoveTo(land);
    waitFreeYielding();

    _action.robot.SetSpeed(7.5f);  // restore crawl default
    _trotPhase = !_trotPhase;
}

void Behavior::tickTrotBackward() {
    waitFreeYielding();
    if (_pending != _state) return;

    _action.robot.SetSpeed(TROT_SPEED);

    RobotLegsPoints swing, land;

    if (!_trotPhase) {
        // Pair A swings to retracted (backward), Pair B slides to extended
        swing = RobotLegsPoints(
            Point(-90.0f, 90.0f - _TS, _TL),   // leg1: lift + step backward
            Point(-90.0f, -90.0f + _TS, 0.0f), // leg2: slide forward (planted)
            Point( 90.0f, 90.0f + _TS, 0.0f),  // leg3: slide forward (planted)
            Point( 90.0f, -90.0f - _TS, _TL)   // leg4: lift + step backward
        );
        land = swing;
        land.leg1.z = 0.0f;
        land.leg4.z = 0.0f;
    } else {
        // Pair B swings backward, Pair A slides forward
        swing = RobotLegsPoints(
            Point(-90.0f, 90.0f + _TS, 0.0f),  // leg1: slide forward (planted)
            Point(-90.0f, -90.0f - _TS, _TL),  // leg2: lift + step backward
            Point( 90.0f, 90.0f - _TS, _TL),   // leg3: lift + step backward
            Point( 90.0f, -90.0f + _TS, 0.0f)  // leg4: slide forward (planted)
        );
        land = swing;
        land.leg2.z = 0.0f;
        land.leg3.z = 0.0f;
    }

    _action.robot.MoveTo(swing);
    waitFreeYielding();
    if (_pending != _state) { _action.robot.SetSpeed(7.5f); return; }

    _action.robot.MoveTo(land);
    waitFreeYielding();

    _action.robot.SetSpeed(7.5f);
    _trotPhase = !_trotPhase;
}

// =============================================================================
//  Servo calibration helpers — called from BrainComm CALI / CSOF commands
// =============================================================================

void Behavior::calibPrep() {
    // Enter the dedicated CALIBRATE behavior state. onEnter() runs
    // Robot::CalibrateState() which lies the body on its belly and raises
    // all four legs above the printed calibration graph with servo offsets
    // disabled. While in this state animations and gait ticks are paused.
    setState(BehaviorState::CALIBRATE);
}

void Behavior::calibNudge(uint8_t leg, int16_t dx, int16_t dy, int16_t dz) {
    // Move a single leg's foot tip relative to its current position.
    // Only honored while in CALIBRATE state to prevent stray nudges from
    // corrupting a gait. Each axis clamped to ±30mm per click as a safety
    // limit — the UI sends 1/5/10mm steps.
    if (_state != BehaviorState::CALIBRATE) return;
    if (leg < 1 || leg > 4) return;
    if (dx < -30) dx = -30; else if (dx > 30) dx = 30;
    if (dy < -30) dy = -30; else if (dy > 30) dy = 30;
    if (dz < -30) dz = -30; else if (dz > 30) dz = 30;
    _action.LegMoveToRelativelyDirectly(leg, Point((float)dx, (float)dy, (float)dz));
}

void Behavior::calibSave() {
    // Freenove-style: auto-compute offsets from current servo angles vs.
    // the theoretical calibrate-point angles, then save to EEPROM.
    // Note: CSOF adjustments are already persisted to EEPROM on each set.
    _action.robot.CalibrateServos();
}

void Behavior::calibCheck() {
    // Move to the 4-point calibration rectangle with offsets applied.
    // All 4 feet should reach the same height if offsets are correct.
    _action.robot.CalibrateVerify();
}

void Behavior::calibReset() {
    // Clear all 12 joint offsets (persists to EEPROM immediately via SetOffset).
    RobotLeg* legs[4] = {
        &_action.robot.leg1, &_action.robot.leg2,
        &_action.robot.leg3, &_action.robot.leg4
    };
    for (uint8_t i = 0; i < 4; ++i) {
        legs[i]->jointA.SetOffset(0.0f);
        legs[i]->jointB.SetOffset(0.0f);
        legs[i]->jointC.SetOffset(0.0f);
    }
}

void Behavior::setServoOffset(uint8_t leg, uint8_t joint, float val) {
    // Map leg (1-4) and joint (0=A, 1=B, 2=C) to the actual joint object.
    if (leg < 1 || leg > 4) return;
    RobotLeg* legs[4] = {
        &_action.robot.leg1, &_action.robot.leg2,
        &_action.robot.leg3, &_action.robot.leg4
    };
    RobotJoint* joints[3] = {
        &legs[leg - 1]->jointA,
        &legs[leg - 1]->jointB,
        &legs[leg - 1]->jointC
    };
    if (joint > 2) return;
    joints[joint]->SetOffset(val);
}

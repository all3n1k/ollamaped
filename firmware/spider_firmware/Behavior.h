#pragma once
#include "FNQRBasic.h"
#include "Config.h"

// =============================================================================
//  Behavior — Spider pet state machine & animations
// =============================================================================

enum class BehaviorState : uint8_t {
    IDLE      = 0,
    ALERT     = 1,
    HAPPY     = 2,
    CURIOUS   = 3,
    SLEEPY    = 4,
    WALKING_F = 5,
    WALKING_B = 6,
    TURNING_L = 7,
    TURNING_R = 8,
    SLEEP     = 9,
    TROT_F    = 10,
    TROT_B    = 11,
    EXCITED   = 12,
    CALIBRATE = 13,
};

// Phase step constants — uint8_t phase wraps at 256 = full circle.
// At 20ms tick: 256 steps / N ticks = cycle length.
// NORMAL  = 6  → 256/6  ≈ 43 ticks = ~860ms cycle
// SLOW    = 3  → 256/3  ≈ 85 ticks = ~1.7s  cycle
// FAST    = 12 → 256/12 ≈ 21 ticks = ~430ms cycle
static constexpr uint8_t PHASE_STEP_NORMAL = 6;

const char* behaviorName(BehaviorState s);

class Behavior {
public:
    explicit Behavior(RobotAction& action) : _action(action) {}

    void begin();
    void update();

    void setState(BehaviorState next);
    BehaviorState getState() const { return _state; }
    bool isBusy() const;

    // Direct brain command overrides
    void moveBody(float x, float y, float z)        { _action.MoveBody(x, y, z); }
    void rotateBody(float x, float y, float z)      { _action.RotateBody(x, y, z); }
    void twistBody(float mx, float my, float mz,
                   float rx, float ry, float rz)    { _action.TwistBody(Point(mx,my,mz), Point(rx,ry,rz)); }
    float voltage() const { return _action.robot.power.voltage; }

    // Animation speed scale (1–10, default 5 = normal)
    void setSpeed(uint8_t s) { _phaseStep = (uint8_t)((uint16_t)PHASE_STEP_NORMAL * s / 5); }

    // Yield hook — BrainComm sets this once in begin() so serial reads
    // happen during blocking gait waits
    void setYieldCallback(void (*cb)()) { _yieldCb = cb; }

    // Servo calibration — called from BrainComm CALI / CSOF commands
    // Freenove flow: PREP raises legs above the printed graph, NUDGE moves
    // each foot in cartesian space until aligned with its mark, SAVE records
    // joint offsets, CHECK verifies, EXIT returns to IDLE.
    void calibPrep();                                         // enter CALIBRATE state — legs raised over graph, offsets disabled
    void calibNudge(uint8_t leg, int16_t dx, int16_t dy, int16_t dz); // move one leg's foot tip by deltas (mm)
    void calibSave();                                         // record current angles as offsets, persist to EEPROM
    void calibCheck();                                        // re-move to calibration marks with offsets applied
    void calibReset();                                        // clear all 12 joint offsets to 0
    void setServoOffset(uint8_t leg, uint8_t joint, float val); // leg=1-4, joint=0(A)/1(B)/2(C)

private:
    RobotAction& _action;

    BehaviorState _state   = BehaviorState::IDLE;
    BehaviorState _pending = BehaviorState::IDLE;

    uint32_t _stateEnteredAt = 0;
    uint32_t _lastTickAt     = 0;
    uint8_t  _phase          = 0;    // 0-255, wraps naturally
    uint8_t  _phaseStep      = PHASE_STEP_NORMAL;
    bool     _trotPhase      = false; // false=pairA swinging, true=pairB swinging

    // Dead-zone tracking for MoveBody
    float _lastX = 0, _lastY = 0, _lastZ = 0;

    void (*_yieldCb)() = nullptr;
    void yieldComm()   { if (_yieldCb) _yieldCb(); }

    uint32_t elapsed() const;

    void onEnter(BehaviorState s);
    void moveBodyDZ(float x, float y, float z);   // dead-zone gated MoveBody
    void waitFreeYielding();

    void tickIdle();
    void tickAlert();
    void tickHappy();
    void tickCurious();
    void tickSleepy();
    void tickWalkForward();
    void tickWalkBackward();
    void tickTurnLeft();
    void tickTurnRight();
    void tickTrotForward();
    void tickTrotBackward();
    void tickExcited();
};

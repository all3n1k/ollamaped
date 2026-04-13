#pragma once
#include <Arduino.h>
#include "Behavior.h"
#include "Buzzer.h"
#include "EspLink.h"

// =============================================================================
//  BrainComm — Robot ↔ Brain serial protocol
//
//  Commands in  (brain → robot, newline terminated):
//    PING | STATUS | STOP | SLEEP | WAKE
//    WALK F|B|L|R
//    TROT F|B
//    BEHAV idle|alert|happy|curious|sleepy|sleep
//    BODY  <x> <y> <z>       (mm, integers, clamped ±30/±20)
//    ROTATE <x> <y> <z>      (degrees * 10)
//    TWIST <mx> <my> <mz> <rx> <ry> <rz>  (mm and degrees, all * 10)
//    SPEED <1-10>            (animation speed, 5=normal)
//    DIAG                    (request diagnostic report)
//
//  State reports out (robot → brain, JSON lines):
//    {"t":"s","v":<mv>,"s":"<state>","b":<0|1>}
//    {"t":"pong"}
//    {"t":"diag","sram":<bytes>,"up":<seconds>}
// =============================================================================

// Yield callback — called by Behavior during blocking gait waits
// so serial reads keep happening. Defined in BrainComm.cpp.
void brainCommYield();

class BrainComm {
public:
    explicit BrainComm(Behavior& behavior) : _behavior(behavior) {}

    void begin();
    void update();

private:
    Behavior& _behavior;

    char     _buf[CMD_BUFFER_SIZE];
    uint8_t  _bufLen      = 0;
    uint32_t _lastReport  = 0;
    uint32_t _lastLedFlip = 0;
    bool     _ledState    = false;
    bool     _sentBoot    = false;
    bool     _lowBatWarn  = false;

    void readSerial(Stream& s);
    void dispatch(const char* cmd);
    void sendState(Stream& s, float voltage);
    void sendPong(Stream& s);
    void sendDiag(Stream& s);
    void sendEvent(Stream& s, const char* evt);
#if WIFI_ENABLED
    void readEspLink();
    void sendStateWifi(float voltage);
    void sendPongWifi();
    void sendDiagWifi();
    void sendEventWifi(const char* evt);
#endif
};

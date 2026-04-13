#include "BrainComm.h"
#include <avr/pgmspace.h>
#include <string.h>

// =============================================================================
//  Static instance pointer — lets the free-function yield callback reach
//  the single BrainComm object without polluting the global namespace.
// =============================================================================
static BrainComm* _instance = nullptr;

void brainCommYield() {
    if (_instance) _instance->update();
    buzzer.update();   // keep tone playing during gait blocking waits
}

// =============================================================================
//  Inline int parser — replaces sscanf("%d %d %d").
//  sscanf on AVR costs ~1-2ms per call and drags in 2KB of float formatting.
//  This does the same job in ~10µs.
// =============================================================================
static int16_t nextInt(const char*& p) {
    while (*p == ' ') ++p;
    bool neg = (*p == '-');
    if (neg) ++p;
    int16_t v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return neg ? -v : v;
}

// =============================================================================
//  Lifecycle
// =============================================================================
void BrainComm::begin() {
    Serial.begin(SERIAL_BAUD);
    pinMode(STATUS_LED_PIN, OUTPUT);
    _instance = this;
    _behavior.setYieldCallback(brainCommYield);
#if WIFI_ENABLED
    espLink.begin();   // takes ownership of Serial2
#else
    Serial2.begin(ESP_BAUD);
#endif
}

// =============================================================================
//  Main update — reads both serial ports, sends periodic state report.
//  Called from loop() and from brainCommYield() during gait waits.
// =============================================================================
void BrainComm::update() {
    readSerial(Serial);
#if WIFI_ENABLED
    espLink.update();
    readEspLink();
#else
    readSerial(Serial2);
#endif

    uint32_t now = millis();

    // Adaptive report rate — back off during gait to avoid buffer pressure.
    BehaviorState st = _behavior.getState();
    bool walking = (st == BehaviorState::WALKING_F ||
                    st == BehaviorState::WALKING_B ||
                    st == BehaviorState::TURNING_L  ||
                    st == BehaviorState::TURNING_R  ||
                    _behavior.isBusy());
    uint16_t interval = walking ? 150 : STATE_REPORT_MS;

    if (now - _lastReport >= interval) {
        _lastReport = now;
        float v = _behavior.voltage();
        sendState(Serial, v);
#if WIFI_ENABLED
        sendStateWifi(v);
#else
        sendState(Serial2, v);
#endif
    }

    // Boot event — sent once after first successful loop tick
    if (!_sentBoot) {
        _sentBoot = true;
        sendEvent(Serial, "boot");
#if WIFI_ENABLED
        sendEventWifi("boot");
#else
        sendEvent(Serial2, "boot");
#endif
    }

    // Low-battery warning event — sent once when voltage crosses VOLTAGE_WARN going down.
    // Resets if voltage recovers (e.g., reconnected to charger / USB).
    float vNow = _behavior.voltage();
    if (!_lowBatWarn && vNow > 0.5f && vNow < VOLTAGE_WARN) {
        _lowBatWarn = true;
        sendEvent(Serial, "low_bat");
#if WIFI_ENABLED
        sendEventWifi("low_bat");
#else
        sendEvent(Serial2, "low_bat");
#endif
    } else if (_lowBatWarn && vNow >= VOLTAGE_WARN) {
        _lowBatWarn = false;   // voltage recovered
    }

    // Heartbeat LED — 500ms toggle
    if (now - _lastLedFlip >= 500) {
        _lastLedFlip = now;
        _ledState = !_ledState;
        digitalWrite(STATUS_LED_PIN, _ledState);
    }
}

// =============================================================================
//  Serial reader — accumulates characters into _buf, dispatches on newline.
// =============================================================================
void BrainComm::readSerial(Stream& s) {
    while (s.available()) {
        char c = (char)s.read();
        if (c == '\n' || c == '\r') {
            if (_bufLen > 0) {
                _buf[_bufLen] = '\0';
                dispatch(_buf);
                _bufLen = 0;
            }
        } else if (_bufLen < CMD_BUFFER_SIZE - 1) {
            _buf[_bufLen++] = c;
        }
    }
}

// =============================================================================
//  Command dispatcher
//  Commands are checked by first character to minimize comparisons.
// =============================================================================
void BrainComm::dispatch(const char* cmd) {
    switch (cmd[0]) {

    case 'P':   // PING
        sendPong(Serial);
#if WIFI_ENABLED
        sendPongWifi();
#else
        sendPong(Serial2);
#endif
        break;

    case 'S':
        if (cmd[1] == 'T') {
            if (cmd[2] == 'O') {                            // STOP
                _behavior.setState(BehaviorState::IDLE);
            } else {                                         // STATUS
                float v = _behavior.voltage();
                sendState(Serial, v);
#if WIFI_ENABLED
                sendStateWifi(v);
#else
                sendState(Serial2, v);
#endif
            }
        } else if (cmd[1] == 'P') {                         // SPEED <1-10>
            const char* p = cmd + 6;
            int16_t s = nextInt(p);
            if (s < 1)  s = 1;
            if (s > 10) s = 10;
            _behavior.setSpeed((uint8_t)s);
        } else {                                             // SLEEP
            _behavior.setState(BehaviorState::SLEEP);
        }
        break;

    case 'W':
        if (cmd[1] == 'A') {                                // WALK F/B/L/R
            switch (cmd[5]) {
                case 'F': _behavior.setState(BehaviorState::WALKING_F); break;
                case 'B': _behavior.setState(BehaviorState::WALKING_B); break;
                case 'L': _behavior.setState(BehaviorState::TURNING_L); break;
                case 'R': _behavior.setState(BehaviorState::TURNING_R); break;
            }
        } else {                                             // WAKE
            _behavior.setState(BehaviorState::IDLE);
        }
        break;

    case 'T':
        if (cmd[1] == 'R') {                                // TROT F|B
            switch (cmd[5]) {
                case 'F': _behavior.setState(BehaviorState::TROT_F); break;
                case 'B': _behavior.setState(BehaviorState::TROT_B); break;
            }
        } else {                                             // TWIST <mx> <my> <mz> <rx> <ry> <rz>
            const char* p = cmd + 6;
            int16_t mx = nextInt(p), my = nextInt(p), mz = nextInt(p);
            int16_t rx = nextInt(p), ry = nextInt(p), rz = nextInt(p);
            _behavior.twistBody(mx * 0.1f, my * 0.1f, mz * 0.1f,
                                rx * 0.1f, ry * 0.1f, rz * 0.1f);
        }
        break;

    case 'B':
        if (cmd[1] == 'E') {                                // BEHAV <state>
            const char* s = cmd + 6;
            switch (s[0]) {
                case 'i': _behavior.setState(BehaviorState::IDLE);    break;
                case 'a': _behavior.setState(BehaviorState::ALERT);   break;
                case 'h': _behavior.setState(BehaviorState::HAPPY);   break;
                case 'c': _behavior.setState(BehaviorState::CURIOUS); break;
                case 'e': _behavior.setState(BehaviorState::EXCITED); break;
                case 's':
                    _behavior.setState(s[2] == 'e' && s[3] == 'e'
                        ? BehaviorState::SLEEPY
                        : BehaviorState::SLEEP);
                    break;
            }
        } else {                                             // BODY <x> <y> <z>
            const char* p = cmd + 5;
            int16_t x = constrain(nextInt(p), -30, 30);
            int16_t y = constrain(nextInt(p), -30, 30);
            int16_t z = constrain(nextInt(p), -20, 20);
            _behavior.moveBody(x, y, z);
        }
        break;

    case 'R': {                                              // ROTATE <x> <y> <z>
        const char* p = cmd + 7;
        int16_t x = nextInt(p);
        int16_t y = nextInt(p);
        int16_t z = nextInt(p);
        _behavior.rotateBody(x * 0.1f, y * 0.1f, z * 0.1f);
        break;
    }

    case 'N': {                                              // BUZZ <pattern_name>
        // Pattern names: boot happy excited curious alert sleepy shy greet ack
        const char* p = cmd + 5;
        while (*p == ' ') ++p;
        switch (p[0]) {
            case 'h': buzzer.play(BuzzPattern::HAPPY);   break;
            case 'e': buzzer.play(BuzzPattern::EXCITED); break;
            case 'c': buzzer.play(BuzzPattern::CURIOUS); break;
            case 'a':
                if (p[1] == 'c') buzzer.play(BuzzPattern::ACK);
                else             buzzer.play(BuzzPattern::ALERT);
                break;
            case 's':
                if (p[2] == 'y') buzzer.play(BuzzPattern::SHY);
                else             buzzer.play(BuzzPattern::SLEEPY);
                break;
            case 'g': buzzer.play(BuzzPattern::GREET);   break;
            case 'b': buzzer.play(BuzzPattern::BOOT);    break;
            default:  buzzer.stop();                     break;
        }
        break;
    }

    case 'D': {                                              // DIAG
        sendDiag(Serial);
#if WIFI_ENABLED
        sendDiagWifi();
#else
        sendDiag(Serial2);
#endif
        break;
    }

    case 'C': {
        if (cmd[1] == 'A') {                                // CALI PREP|SAVE|CHECK|RESET|NUDGE|EXIT
            const char* sub = cmd + 5;                      // skip "CALI "
            if      (sub[0] == 'P') _behavior.calibPrep();
            else if (sub[0] == 'S') _behavior.calibSave();
            else if (sub[0] == 'C') _behavior.calibCheck();
            else if (sub[0] == 'R') _behavior.calibReset();
            else if (sub[0] == 'E') _behavior.setState(BehaviorState::IDLE);
            else if (sub[0] == 'N') {                       // CALI NUDGE <leg> <dx> <dy> <dz>
                const char* p = sub + 6;                    // skip "NUDGE "
                int16_t leg = nextInt(p);
                int16_t dx  = nextInt(p);
                int16_t dy  = nextInt(p);
                int16_t dz  = nextInt(p);
                _behavior.calibNudge((uint8_t)leg, dx, dy, dz);
            }
        } else {                                             // CSOF L J V  (set servo offset)
            // Format: "CSOF <leg 1-4> <joint 0-2> <value×10>"
            const char* p = cmd + 5;                        // skip "CSOF "
            int16_t leg   = nextInt(p);
            int16_t joint = nextInt(p);
            int16_t val10 = nextInt(p);
            _behavior.setServoOffset((uint8_t)leg, (uint8_t)joint, val10 * 0.1f);
        }
        break;
    }

    default: break;
    }
}

// =============================================================================
//  Output — build JSON into a stack buffer, single Serial.write() call.
//  Format: {"t":"s","v":XXX,"s":"YYYY","b":X}\n
// =============================================================================
static char* writeInt(char* p, int v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char tmp[6]; int8_t n = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0) *p++ = tmp[--n];
    return p;
}

void BrainComm::sendState(Stream& s, float voltage) {
    // {"t":"s","v":XXX,"s":"YYYY","b":X}\n
    // Max length: 4+3+5+7+5+1+2 = ~40 bytes. buf[48] is safe.
    char buf[48];
    char* p = buf;

    memcpy_P(p, PSTR("{\"t\":\"s\",\"v\":"), 13); p += 13;
    p = writeInt(p, (int)(voltage * 100.0f));
    memcpy_P(p, PSTR(",\"s\":\""), 6);             p += 6;
    const char* name = behaviorName(_behavior.getState());
    while (*name) *p++ = *name++;
    memcpy_P(p, PSTR("\",\"b\":"), 6);             p += 6;
    *p++ = _behavior.isBusy() ? '1' : '0';
    memcpy_P(p, PSTR("}\n"), 2);                   p += 2;

    s.write((const uint8_t*)buf, (size_t)(p - buf));
}

void BrainComm::sendPong(Stream& s) {
    // Literal bytes — no formatting needed at all
    static const uint8_t pong[] PROGMEM = "{\"t\":\"pong\"}\n";
    uint8_t buf[14];
    memcpy_P(buf, pong, 14);
    s.write(buf, 14);
}

// =============================================================================
//  Free SRAM — measures stack bottom against heap top.
//  On AVR with no dynamic allocation, __brkval is null and we measure against
//  __heap_start.  Returns bytes available for stack growth.
// =============================================================================
static int freeRam() {
    extern int __heap_start, *__brkval;
    int v;
    return (int)&v - (__brkval == nullptr ? (int)&__heap_start : (int)__brkval);
}

void BrainComm::sendDiag(Stream& s) {
    // {"t":"diag","sram":NNNNN,"up":NNNNNN}\n  ≤ 48 bytes
    char buf[48];
    char* p = buf;
    memcpy_P(p, PSTR("{\"t\":\"diag\",\"sram\":"), 19); p += 19;
    p = writeInt(p, freeRam());
    memcpy_P(p, PSTR(",\"up\":"), 6);                    p += 6;
    p = writeInt(p, (int)(millis() / 1000UL));
    memcpy_P(p, PSTR("}\n"), 2);                         p += 2;
    s.write((const uint8_t*)buf, (size_t)(p - buf));
}

void BrainComm::sendEvent(Stream& s, const char* evt) {
    // {"t":"evt","e":"XXXXX"}\n — max 32 bytes
    char buf[32];
    char* p = buf;
    memcpy_P(p, PSTR("{\"t\":\"evt\",\"e\":\""), 16); p += 16;
    while (*evt) *p++ = *evt++;
    memcpy_P(p, PSTR("\"}\n"), 3);                    p += 3;
    s.write((const uint8_t*)buf, (size_t)(p - buf));
}

// =============================================================================
//  WiFi variants — use EspLink instead of a Stream
// =============================================================================
#if WIFI_ENABLED

void BrainComm::readEspLink() {
    while (espLink.available()) {
        int c = espLink.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (_bufLen > 0) {
                _buf[_bufLen] = '\0';
                dispatch(_buf);
                _bufLen = 0;
            }
        } else if (_bufLen < CMD_BUFFER_SIZE - 1) {
            _buf[_bufLen++] = (char)c;
        }
    }
}

void BrainComm::sendStateWifi(float voltage) {
    char buf[48];
    char* p = buf;
    memcpy_P(p, PSTR("{\"t\":\"s\",\"v\":"), 13); p += 13;
    p = writeInt(p, (int)(voltage * 100.0f));
    memcpy_P(p, PSTR(",\"s\":\""), 6);             p += 6;
    const char* name = behaviorName(_behavior.getState());
    while (*name) *p++ = *name++;
    memcpy_P(p, PSTR("\",\"b\":"), 6);             p += 6;
    *p++ = _behavior.isBusy() ? '1' : '0';
    memcpy_P(p, PSTR("}\n"), 2);                   p += 2;
    espLink.write((const uint8_t*)buf, (size_t)(p - buf));
}

void BrainComm::sendPongWifi() {
    static const uint8_t pong[] PROGMEM = "{\"t\":\"pong\"}\n";
    uint8_t buf[14];
    memcpy_P(buf, pong, 14);
    espLink.write(buf, 14);
}

void BrainComm::sendEventWifi(const char* evt) {
    char buf[32];
    char* p = buf;
    memcpy_P(p, PSTR("{\"t\":\"evt\",\"e\":\""), 16); p += 16;
    while (*evt) *p++ = *evt++;
    memcpy_P(p, PSTR("\"}\n"), 3);                    p += 3;
    espLink.write((const uint8_t*)buf, (size_t)(p - buf));
}

void BrainComm::sendDiagWifi() {
    char buf[48];
    char* p = buf;
    memcpy_P(p, PSTR("{\"t\":\"diag\",\"sram\":"), 19); p += 19;
    p = writeInt(p, freeRam());
    memcpy_P(p, PSTR(",\"up\":"), 6);                    p += 6;
    p = writeInt(p, (int)(millis() / 1000UL));
    memcpy_P(p, PSTR("}\n"), 2);                         p += 2;
    espLink.write((const uint8_t*)buf, (size_t)(p - buf));
}

#endif // WIFI_ENABLED

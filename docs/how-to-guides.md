# Spider Robot — How-To Guides

Practical recipes for modifying Spider's behavior. Read these when you want to change
something specific and don't know where to start.

---

## Table of Contents

1. [How the data flows end-to-end](#1-how-the-data-flows-end-to-end)
2. [Add a new firmware animation/behavior](#2-add-a-new-firmware-animationbehavior)
3. [Add a new brain action (LLM-callable)](#3-add-a-new-brain-action-llm-callable)
4. [Tune the emotion system](#4-tune-the-emotion-system)
5. [Add a new buzzer sound pattern](#5-add-a-new-buzzer-sound-pattern)
6. [Add a new REST API endpoint](#6-add-a-new-rest-api-endpoint)
7. [Change how autonomy behaves](#7-change-how-autonomy-behaves)
8. [Compile and flash the firmware](#8-compile-and-flash-the-firmware)
9. [Run the brain server](#9-run-the-brain-server)
10. [Enable WiFi mode (when ESP-01S arrives)](#10-enable-wifi-mode-when-esp-01s-arrives)
11. [Wire the buzzer](#11-wire-the-buzzer)
12. [Useful debugging tips](#12-useful-debugging-tips)

---

## 1. How the data flows end-to-end

Understanding the full data path helps you know which file to edit.

```
[You / LLM / UI browser]
        │
        ▼
brain/src/main.py          ← FastAPI HTTP/WebSocket server
        │
        ▼
brain/src/robot/actions.py ← translate named actions into commands
        │
        ▼
brain/src/robot/comm.py    ← queue a command string (e.g. "TROT F\n")
        │  USB serial or TCP
        ▼
firmware/BrainComm.cpp     ← receive and dispatch the command
        │
        ▼
firmware/Behavior.cpp      ← run the animation / gait tick
        │
        ▼
firmware/FNQRBasic.cpp     ← IK engine moves servos to target positions
        │
        ▼
[Physical servos on Spider]
```

In the other direction (robot → brain):
```
[firmware voltage ADC / state machine]
        │
        ▼
BrainComm.cpp sendState()  ← builds JSON like {"t":"s","v":734,"s":"idle","b":0}
        │  USB serial or TCP
        ▼
brain/src/robot/comm.py    ← _parse_line() → robot_state.update_from_report()
        │
        ▼
brain/src/robot/state.py   ← robot_state singleton updated
        │
        ├── event callback → emotion_engine.stimulate() if {"t":"evt"}
        │
        ▼
main.py state_broadcast_task → WebSocket → browser UI
```

---

## 2. Add a new firmware animation/behavior

**Example goal**: add a `SHAKE` state where Spider rapidly shakes its body side to side.

### Step 1 — Add the state enum (Behavior.h)

```cpp
enum class BehaviorState : uint8_t {
    // ... existing states ...
    EXCITED   = 12,
    SHAKE     = 13,   // ← add here
};
```

### Step 2 — Add the tick method declaration (Behavior.h)

```cpp
private:
    // ... existing private methods ...
    void tickShake();    // ← add this
```

### Step 3 — Implement the tick (Behavior.cpp)

```cpp
void Behavior::tickShake() {
    // _phase advances by _phaseStep every 20ms tick
    // sinTab[_phase >> 2] gives a value -127..+127 from the PROGMEM sine table
    float roll = sinTab[_phase >> 2] * 0.08f;   // ±~10mm side-to-side
    moveBodyDZ(roll, 0, 0);
    _phase += _phaseStep * 2;   // twice normal speed
}
```

Look at `tickHappy()` or `tickExcited()` in Behavior.cpp for examples of how the sine table and `_phase` work.

### Step 4 — Wire it into `update()` (Behavior.cpp)

Find the `switch (_state)` block in `Behavior::update()`:
```cpp
case BehaviorState::EXCITED:   tickExcited();   break;
case BehaviorState::SHAKE:     tickShake();     break;   // ← add
```

### Step 5 — Wire it into `onEnter()` (Behavior.cpp)

If your state needs initialization (reset phase, reset position), add it:
```cpp
case BehaviorState::SHAKE:
    _phase = 0;
    break;
```

### Step 6 — Add the behavior name (Behavior.cpp)

Find the `behaviorName()` function:
```cpp
case BehaviorState::SHAKE: return "shake";
```

### Step 7 — Add the BEHAV command dispatch (BrainComm.cpp)

Find `case 'B':` → `case 'E':` section in `dispatch()`:
```cpp
case 'B':
    if (cmd[1] == 'E') {    // BEHAV
        const char* s = cmd + 6;
        switch (s[0]) {
            // ... existing cases ...
            case 'sh': _behavior.setState(BehaviorState::SHAKE); break;
            // ↑ Note: first char 's' conflicts with 'sleepy'/'sleep'
            //   Need to use s[2] or s[3] to disambiguate:
            case 's':
                if      (s[1]=='h') _behavior.setState(BehaviorState::SHAKE);
                else if (s[2]=='e' && s[3]=='e') _behavior.setState(BehaviorState::SLEEPY);
                else    _behavior.setState(BehaviorState::SLEEP);
                break;
        }
    }
```

### Step 8 — Expose it in the brain (comm.py)

The `set_behavior()` method in `brain/src/robot/comm.py` just sends `"BEHAV {state}"`, so it works automatically — just call `robot_comm.set_behavior("shake")`.

---

## 3. Add a new brain action (LLM-callable)

**Example goal**: add a `nod` action that bows the head twice.

### Step 1 — Write the async function (actions.py)

Add near the other body language actions:

```python
async def nod() -> str:
    """
    Nod twice — a brief up-down head motion expressing agreement or acknowledgement.
    """
    for _ in range(2):
        await robot_comm.rotate_body(80, 0, 0)   # 8° forward pitch
        await asyncio.sleep(0.25)
        await robot_comm.rotate_body(0, 0, 0)
        await asyncio.sleep(0.20)
    return "nodded"
```

### Step 2 — Register it in the dispatcher (actions.py)

Find `_ACTIONS` dict and add:
```python
"nod": nod,
```

### Step 3 — Add the tool schema (actions.py)

Add an entry to `TOOLS`:
```python
{
    "name": "nod",
    "description": "Nod twice — expresses agreement or acknowledgement.",
    "input_schema": {"type": "object", "properties": {}},
},
```

That's it. Now:
- `POST /api/llm/act` with `{"action": "nod", "params": {}}` executes it
- `GET /api/llm/tools` includes it in the schema
- The LLM system prompt will reference it automatically via `get_state()`

---

## 4. Tune the emotion system

All emotion tuning lives in `brain/src/robot/emotion.py`.

### Change how fast emotions fade

```python
class EmotionEngine:
    VALENCE_DECAY = 0.006   # ← higher = fades faster; 0.006 ≈ 60% in ~90s
    AROUSAL_DECAY = 0.010   # ← higher = fades faster; 0.010 ≈ 60% in ~60s
```

These are fractions-per-second applied toward the baseline. 0.006/s means valence moves 0.6% of the remaining gap per second toward baseline. After 90 seconds a full +1.0 stimulus decays to ~+0.42.

### Change how strong a stimulus is

Edit the `STIMULI` dict:
```python
STIMULI: dict[str, tuple[float, float]] = {
    "person_seen": (0.40, 0.30),    # (dValence, dArousal)
    ...
}
```
Values are added to the current valence/arousal, then clamped to [-1, +1]. If you want Spider to react more strongly when someone is seen, raise these. To make it less reactive, lower them.

### Add a new stimulus

Add an entry to `STIMULI`:
```python
"ball_seen": (0.25, 0.40),   # playful excitement on seeing a ball
```

Then fire it anywhere in the brain:
```python
from robot.emotion import emotion_engine
emotion_engine.stimulate("ball_seen")
```

### Change baseline mood by time of day

Edit `_BASELINES`:
```python
_BASELINES = [
    ((6,  10), ( 0.20,  0.15)),   # morning
    ((10, 18), ( 0.15,  0.00)),   # daytime
    # etc.
]
```
Values are (baseline_valence, baseline_arousal). Emotions decay toward these over time.

### Change what emotion label maps to what firmware behavior

Edit `_FIRMWARE_BEHAVIOR`:
```python
_FIRMWARE_BEHAVIOR: dict[str, str] = {
    "excited": "excited",
    "happy":   "happy",
    ...
}
```
The values must match firmware behavior name strings (e.g. `"idle"`, `"curious"`, `"happy"`).

---

## 5. Add a new buzzer sound pattern

All buzzer patterns live in `firmware/spider_firmware/Buzzer.cpp`.

### Step 1 — Add the pattern array (Buzzer.cpp)

```cpp
// Format: NOTE(freq_hz, duration_ms), REST(duration_ms), END
// All values are uint16_t stored in PROGMEM.
static const uint16_t PROGMEM _pat_playful[] = {
    NOTE(523, 60), NOTE(659, 60), NOTE(784, 60),
    REST(30),
    NOTE(784, 60), NOTE(659, 60), NOTE(523, 90),
    END
};
```

Musical note reference (approx Hz):
`C4=262, D4=294, E4=330, F4=349, G4=392, A4=440, B4=494, C5=523, D5=587, E5=659, F5=698, G5=784, A5=880, B5=988, C6=1047`

### Step 2 — Add enum value (Buzzer.h)

```cpp
enum class BuzzPattern : uint8_t {
    BOOT=0, HAPPY, EXCITED, CURIOUS, ALERT, SLEEPY, SHY, GREET, ACK,
    PLAYFUL,   // ← add new entry; value = 9
};
```

### Step 3 — Add to pattern table (Buzzer.cpp)

```cpp
static const uint16_t* const _patterns[] PROGMEM = {
    _pat_boot, _pat_happy, _pat_excited, _pat_curious,
    _pat_alert, _pat_sleepy, _pat_shy, _pat_greet, _pat_ack,
    _pat_playful,   // ← same position as enum value (index 9)
};
```

### Step 4 — Add BUZZ command dispatch (BrainComm.cpp)

Find `case 'N':` (the BUZZ handler) and add a case for the first character:
```cpp
case 'p': buzzer.play(BuzzPattern::PLAYFUL); break;
```

If there's a character conflict (e.g. another pattern starts with 'p'), disambiguate with `p[1]`.

### Step 5 — Add to the brain (comm.py and actions.py)

`comm.py` just sends `"BUZZ playful\n"` — no change needed (the `buzz()` method sends whatever string you pass).

Add `"playful"` to the valid set in `chirp()` in `actions.py`:
```python
valid = {"boot", "happy", "excited", "curious", "alert", "sleepy", "shy", "greet", "ack", "playful"}
```

Also update the TOOLS schema enum list for `chirp`.

---

## 6. Add a new REST API endpoint

All endpoints are in `brain/src/main.py`.

**Example**: add `POST /api/cmd/nod` endpoint.

```python
@app.post("/api/cmd/nod")
async def cmd_nod():
    result = await robot_actions.dispatch("nod", {})
    return {"ok": True, "result": result}
```

For endpoints with a body, use Pydantic:
```python
class NodRequest(BaseModel):
    count: int = 2

@app.post("/api/cmd/nod")
async def cmd_nod(body: NodRequest):
    result = await robot_actions.dispatch("nod", {"count": body.count})
    return {"ok": True, "result": result}
```

---

## 7. Change how autonomy behaves

All autonomy config is in `brain/src/robot/autonomy.py`.

### Enable LLM-driven autonomy (Ollama)

Install Ollama from https://ollama.com, then:
```
ollama pull llama3.2:3b        # 2GB RAM, ~0.5s/response — recommended
ollama pull llama3.1:8b        # 5GB RAM, ~2s/response — smarter
ollama pull gemma3:12b         # 7GB RAM, ~4s/response — very good
```

Start Ollama (`ollama serve`), then start the brain. Autonomy will
automatically use Ollama. Override the model via env var:
```
set OLLAMA_MODEL=llama3.1:8b
start.bat
```

If Ollama is not running, autonomy silently falls back to the rule-based picker.

### Change tick rate or lockout window

```python
_TICK_INTERVAL   = 15.0   # seconds between autonomy decisions — raise to make Spider more passive
_COMMAND_LOCKOUT = 40.0   # seconds after human command before autonomy resumes
_ACTION_PROB     = 0.60   # 60% chance of acting per tick — raise for more active Spider
```

### Change what actions Spider does autonomously

Edit `_pick_action()`. Each emotion label has a list of `(action_name, params)` tuples.
`random.choice()` picks one per tick. Add, remove, or change entries:

```python
if label == "idle":
    return random.choice([
        ("tilt_head", {"side": random.choice(["left", "right"])}),
        ("look_at",   {"direction": random.choice(["left", "right"])}),
        ("stretch",   {}),
        ("center",    {}),
        ("bow",       {}),
        ("chirp",     {"pattern": "ack"}),   # ← add a new option
    ])
```

### Disable autonomy entirely

```python
from robot.autonomy import autonomy_engine
autonomy_engine.set_enabled(False)
```

Or just don't start the task in `main.py` lifespan.

---

## 8. Compile and flash the firmware

### Prerequisites
- `arduino-cli` at `C:\arduino-cli\arduino-cli.exe`
- ATmega2560 board package installed
- FNQRBasic library installed (or in the sketch folder)

### Compile only
```
C:\arduino-cli\arduino-cli.exe compile --fqbn arduino:avr:mega firmware/spider_firmware
```

Watch for:
- Flash usage should stay under 80% (204KB of 256KB)
- SRAM globals should stay under 70% (5.6KB of 8KB)
- Any `multiple definition` linker errors mean a timer ISR conflict

### Compile and upload
```
C:\arduino-cli\arduino-cli.exe compile --fqbn arduino:avr:mega --upload -p COM3 firmware/spider_firmware
```

Replace `COM3` with the actual port. The brain server must be stopped first — it will have the port open.

### After flashing
Spider should immediately:
1. Play the BOOT chirp (three rising tones) — only if buzzer is wired
2. Start sending state reports over USB

---

## 9. Run the brain server

```
cd C:\Users\Admin\Downloads\spider-robot\brain
start.bat
```

Or manually:
```
cd brain
.venv\Scripts\activate
cd src
python main.py
```

Open `http://localhost:8080` for the dashboard.

**Environment variables** you can set before running:
- `ROBOT_PORT=COM3` — serial port (default COM3)
- `ROBOT_TCP_SERVER=1` — opt into an incoming ESP8266 connection (requires `ROBOT_TCP_BIND` and `ROBOT_TCP_PEER`)
- `SERVER_PORT=8080` — web server port

### Test the API manually
```
# Check state
curl http://localhost:8080/api/state

# Make Spider greet
curl -X POST http://localhost:8080/api/llm/act -H "Content-Type: application/json" -d "{\"action\":\"greet\",\"params\":{}}"

# Fire an emotion stimulus
curl -X POST http://localhost:8080/api/emotion/stimulate/person_seen

# Get all available actions
curl http://localhost:8080/api/llm/tools
```

---

## 10. Enable WiFi mode (when ESP-01S arrives)

When the ESP-01S programmer and ESP32-CAM arrive, follow `memory/project_hardware_arrival_instructions.md` for the full step-by-step. Quick summary:

1. Flash ESP-01S with `firmware/spider_esp/spider_esp.ino` using the programmer
2. Edit `firmware/spider_firmware/Config.h`:
   ```cpp
   #define WIFI_ENABLED  1
   #define WIFI_SSID     "YourNetwork"
   #define WIFI_PASSWORD "YourPassword"
   #define BRAIN_HOST    "192.168.1.XXX"   // Optiplex LAN IP
   #define BRAIN_PORT    9000
   ```
3. Recompile and flash the ATmega firmware
4. Set `ROBOT_TCP_SERVER=1`, `ROBOT_TCP_BIND` (the brain interface IP), and `ROBOT_TCP_PEER` (the ESP8266 IP) before launching
5. Robot will connect WiFi → TCP to brain on boot; USB serial still works as fallback

---

## 11. Wire the buzzer

The firmware is already complete. You just need the hardware.

**What to buy**: a "passive piezo buzzer module" with 3 dupont header pins (S/+/-).
These cost ~$2 on Amazon/AliExpress and come with pre-soldered 2.54mm headers.

**Do NOT buy an active buzzer** — active buzzers have a fixed internal tone and ignore the signal frequency. You need passive (no internal oscillator).

**Wiring** (P3 connector on the robot board):
- Buzzer **S** (signal) → P3 **A0** pin
- Buzzer **−** or **GND** → P3 **GND** pin
- Buzzer **+** or **VCC** → P3 **3.3V** or **5V** (either works for most modules)

After wiring, flash the firmware (unchanged) and power on — you should hear three rising tones.

---

## 12. Useful debugging tips

### Check what state Spider is in
```
curl http://localhost:8080/api/state
```
Look at `behavior` (what firmware animation is running), `busy` (is a gait active), and `emotion` (current emotional state).

### Manually trigger any emotion
```
curl -X POST http://localhost:8080/api/emotion/stimulate/celebration
```
Watch the behavior change live in the dashboard.

### Check SRAM on the ATmega
```
curl -X POST http://localhost:8080/api/cmd/diag
curl http://localhost:8080/api/state   # look at free_sram field
```
If `free_sram` is below ~500 bytes, the firmware is close to a stack overflow. Reduce buffer sizes or static allocations.

### Serial monitor (when brain is stopped)
Open Arduino IDE serial monitor at 115200 baud on COM3.
You will see the JSON state reports every 50ms, plus `{"t":"evt","e":"boot"}` on startup.
Type commands manually: `PING`, `BEHAV happy`, `BUZZ greet`, etc.

### Firmware won't compile: timer conflict
If you see `multiple definition of '__vector_XX'`, you've added an ISR that conflicts with the Servo or FlexiTimer2 libraries. The ATmega2560 has no free hardware timers — use `micros()` polling like the buzzer does.

### Brain crashes with `AttributeError` on startup
The brain imports singletons from `robot/emotion.py`, `robot/autonomy.py`, etc. If a file has a syntax error these imports fail silently-ish. Run `python -c "from robot.emotion import emotion_engine"` in the `src/` directory to isolate.

### Robot moves erratically at low battery
At < 6.5V the servos get weak and the IK targets are unreachable. The firmware auto-sleeps at 5.5V (`VOLTAGE_OFF`). If you're testing on USB only, `DEV_MODE` bypasses all voltage checks.

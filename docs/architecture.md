# Ollamaped architecture

## Hardware

| Component | Detail |
|-----------|--------|
| Main MCU | ATmega2560 @ 16 MHz, 256 KB flash, 8 KB SRAM |
| WiFi module | ESP8266EX (ESP-01S, 1 MB flash, P25Q80H) on a 4×2 socket |
| Servos | 12× (3 per leg × 4 legs), driven by FlexiTimer2 ISR at 20 ms |
| Power | 2× 14500 Li-ion, 3 MOSFET power groups (A15, A13, A14) |
| Board | Freenove FNM0019 V2 (robot kit V2) |
| Buzzer | Passive piezo on P3 A0 (PF0) |
| Brain | Any desktop on the LAN (Optiplex i5-12500T / 16 GB here) |
| Camera | ESP32-CAM over LAN (OTA-flashable after first flash) |

### Key hardware quirks

- **GPIO0 of the ESP-01S is hardwired to 3.3V on the V2 board** via pull-up
  resistors — it is not connected to any ATmega pin. Software bootloader
  mode on the ESP is physically impossible on this board. A one-time
  physical flash with a USB programmer is required.
- **The NRF24L01 connector (P1) is populated but unused.** We stripped
  radio code entirely. The SPI bus is free for future use.
- **P3 (2×8 female) exposes** 3.3V, 5V, 6× GND, A0 (buzzer), A1, SDA(20),
  SCL(21), TX3(14), RX3(15). This is where the camera and any future
  sensors (IMU, mic, ToF) get wired.

---

## Architecture

```
┌──────────────────────────┐          ASCII JSON lines            ┌──────────────────────────┐
│  ATmega2560 firmware     │ ◀──────── USB serial / WiFi ────────▶│  Brain server (Python)   │
│  (C++ / Arduino)         │                                      │  FastAPI + WebSocket     │
│                          │                                      │                          │
│  ┌────────────────────┐  │                                      │  ┌────────────────────┐  │
│  │ Behavior SM (14)   │  │                                      │  │ robot_state        │  │
│  │ IK engine          │  │                                      │  │ emotion engine     │  │
│  │ Polling buzzer     │  │                                      │  │ autonomy tick loop │  │
│  │ EspLink AT driver  │  │                                      │  │ perception context │  │
│  │ Manual JSON parser │  │                                      │  │ action dispatch    │  │
│  └────────────────────┘  │                                      │  └─────────┬──────────┘  │
└──────────────────────────┘                                      └────────────┼─────────────┘
                                                                               │
                                           ┌───────────────────────────────────┤
                                           ▼                                   ▼
                                ┌──────────────────────┐            ┌──────────────────────┐
                                │ Ollama (local LLM)   │            │ ESP32-CAM (LAN HTTP) │
                                │ llama3.2:3b default  │            │ /capture /stream     │
                                └──────────────────────┘            └──────────────────────┘

                                                                               ▲
                                                                               │
                                                                ┌──────────────┴──────────────┐
                                                                │ Dashboard (browser)         │
                                                                │ Three.js 3D viewport,       │
                                                                │ status, logs, cal panel     │
                                                                └─────────────────────────────┘
```

**Architectural split:** the ATmega does nothing but low-level servo
interpolation, IK, gait generation, and a simple command protocol. All
intelligence — LLM reasoning, emotion modelling, perception, vision,
autonomy — runs on the brain server. This is the correct split for the
hardware: the MCU has 8 KB of SRAM and no FPU, so trying to run anything
LLM-flavored on it is hopeless. The brain is the smart layer, and the
firmware is a dumb, fast, reliable actuator.

---

## Repo layout

```
ollamaped/
├── firmware/
│   ├── spider_firmware/         ← ATmega2560 main firmware (C++ / Arduino)
│   │   ├── spider_firmware.ino  ← entry point, setup/loop
│   │   ├── Config.h             ← ALL pins, tuning constants, build flags
│   │   ├── Secrets.example.h    ← template — copy to Secrets.h
│   │   ├── Behavior.h / .cpp    ← 14-state animation machine
│   │   ├── BrainComm.h / .cpp   ← serial + WiFi command protocol
│   │   ├── Buzzer.h / .cpp      ← non-blocking polling buzzer driver
│   │   ├── EspLink.h / .cpp     ← AT-command driver for ESP-01S
│   │   └── FNQRBasic.h / .cpp   ← Freenove IK engine (kept verbatim)
│   ├── spider_esp/              ← ESP-01S custom firmware (WiFi bridge + OTA)
│   ├── esp_passthrough/         ← one-time ATmega→ESP serial passthrough
│   ├── esp_at_bridge/           ← helper: ESP acts as AT responder for testing
│   └── esp_restore/             ← helper: reflash a bricked ESP
├── tools/
│   ├── esp32cam_firmware/       ← ESP32-CAM streaming + OTA firmware
│   └── tft_probe/               ← TFT display probe sketch
├── brain/
│   ├── start.bat                ← launch script (Windows)
│   ├── requirements.txt
│   └── src/
│       ├── main.py              ← FastAPI app, WebSocket, routes, lifespan
│       ├── robot/
│       │   ├── comm.py          ← serial + TCP connection manager
│       │   ├── state.py         ← robot state dataclass + singleton
│       │   ├── emotion.py       ← circumplex emotion engine
│       │   ├── autonomy.py      ← Ollama-driven autonomy tick loop
│       │   ├── perception.py    ← builds LLM context string
│       │   ├── actions.py       ← 21 LLM-callable actions + schema
│       │   ├── pose.py          ← pose helpers
│       │   └── vision_prompt.py ← vision prompt shaping
│       ├── vision/              ← optional vision backends
│       └── ui/
│           ├── index.html       ← dashboard (Three.js via CDN importmap)
│           └── static/
│               ├── css/style.css
│               └── js/app.js    ← WebSocket client, 3D viz, controls
├── docs/
│   └── how-to-guides.md
└── README.md                    ← you are here
```

---

## Firmware deep dive

### Timer allocation — critical

Every hardware timer on the ATmega2560 is claimed at compile time:

| Timer            | Owner                 | Notes |
|------------------|-----------------------|-------|
| Timer0           | Arduino `millis()`    | System — cannot touch |
| Timer1, 3, 4, 5  | Arduino Servo library | Claims **all** of them at link time, even if only 12 servos are attached |
| Timer2           | FlexiTimer2           | Drives the 20 ms servo interpolation ISR |

Result: **there are zero free timers**. Any attempt to use one of the
Servo-owned timer ISRs for a tone or PWM causes a linker error:

```
multiple definition of '__vector_42'
```

That's why the buzzer is driven by **polling** instead of a timer ISR —
see [Buzzer driver](#buzzer-driver) below.

### State machine (14 states)

`Behavior` owns what the robot is currently doing. Transitions are
"pending-flagged": `setState()` sets `_pending`, and `update()` only
actually switches when `!isBusy()` — this prevents interruption of
blocking gait sections mid-animation.

| ID | State        | Description |
|----|--------------|-------------|
| 0  | IDLE         | Gentle breathing sway |
| 1  | ALERT        | Raised, tense scan |
| 2  | HAPPY        | Fast excited wiggle |
| 3  | CURIOUS      | Gentle body lean / tilt |
| 4  | SLEEPY       | Slow drooping sway — auto-transitions to SLEEP after 10 s |
| 5  | WALKING_F    | Crawl gait forward |
| 6  | WALKING_B    | Crawl gait backward |
| 7  | TURNING_L    | Turn left (CCW) |
| 8  | TURNING_R    | Turn right (CW) |
| 9  | SLEEP        | Folded / motors relaxed |
| 10 | TROT_F       | Diagonal-pair trot forward |
| 11 | TROT_B       | Diagonal-pair trot backward |
| 12 | EXCITED      | Fast body waggle |
| 13 | CALIBRATE    | Belly-down, legs raised — used by the browser cal UI |

Implementation notes:

- **20 ms tick rate**, aligned with the servo ISR
- **64-entry `int8_t` PROGMEM sine table**, phase is `uint8_t` so it wraps
  naturally — replaces `sinf()` (~2 µs vs ~200 µs per evaluation on AVR)
- **`moveBodyDZ()`** dead-zones MoveBody by 0.5 mm on all axes to skip
  redundant IK calls
- **`waitFreeYielding()`** loops `yieldComm()` during blocking gait calls
  so serial commands don't get dropped while walking
- **Trot uses absolute-position targets**, not relative offsets — relative
  offsets drift across cycles, absolute targets make the 2-phase
  (leg1+leg4, then leg2+leg3) cycle perfectly repeatable

### BrainComm — protocol handler

Hot-path design choices, every one of them measured against the
alternatives:

- **Reads both `Serial` (USB) and `Serial2` (WiFi) every loop** — no mode flag
- **First-char `switch` dispatch** on `cmd[0]` — O(1) command routing
- **Inline `nextInt()`** parser instead of `sscanf` — ~10 µs vs ~2 ms, and
  saves ~1 KB of flash
- **Manual `writeInt()` + `memcpy_P()` JSON writer** — single `Serial.write()`
  per frame, no heap, no `sprintf` (which is another 1–2 KB of flash and
  several ms per call)
- **Adaptive report rate** — 50 ms idle, 150 ms during walking/trot. Cuts
  bandwidth when there's nothing interesting to say
- **Sticky events** — `{"t":"evt","e":"boot"}` fires once on first loop
  tick, `{"t":"evt","e":"low_bat"}` fires once when voltage crosses 7.0 V
  downward

### Buzzer driver

9 emotion patterns shipped: `boot`, `happy`, `excited`, `curious`,
`alert`, `sleepy`, `shy`, `greet`, `ack`.

- **No ISR**, no timer — `update()` uses `micros()` polling from the main loop
- **`PINF = (1 << PF0)`** — the write-to-PIN-register AVR trick toggles the
  output pin in ~2 cycles
- **Pattern table in PROGMEM** as pairs of `(freq_hz, duration_ms)`,
  terminated by `(0, 0)`. A pattern struct uses `NOTE(f, d)`, `REST(d)`,
  `END` macros, where `REST` is encoded as `freq = 1` (pin stays low)
- **Also ticked from `brainCommYield()`** so tones keep playing during
  blocking gait calls inside the IK engine

### FNQRBasic — IK engine

Kept verbatim from the Freenove reference. Floating-point 4-leg 3-DOF IK.
`RobotAction` exposes:

- `CrawlForward()`, `CrawlBackward()`, `TurnLeft()`, `TurnRight()`
- `MoveBody(x, y, z)`, `RotateBody(x, y, z)`, `TwistBody(move, rot)`
- `MoveTo(RobotLegsPoints)` — move all 4 legs to explicit absolute positions
- `ActiveMode()`, `SleepMode()`, `InitialState()`, `CalibrateState()`

EEPROM joint-offset calibration is stored at addresses 100–128 (12 joints × 2
bytes). `CalibrateServos()` computes offsets as `current_angle - theoretical_angle`
and persists each immediately via `RobotJoint::SetOffset()`.

### EspLink — WiFi via stock AT firmware

When `WIFI_ENABLED=1`, the ATmega configures the stock Espressif AT firmware
on the ESP-01S via `AT+RST → AT+CWMODE_CUR=1 → AT+CWJAP_CUR → AT+CIPMUX=0
→ AT+CIPSTART`, then switches to transparent passthrough. Incoming
`+IPD,N:` headers are parsed into a ring buffer on read. The brain can
also drive the ESP-01S directly via its own custom firmware in
`firmware/spider_esp/` — see [WiFi untethering](#wifi-untethering).

---

## Brain server deep dive

- **FastAPI** app, **WebSocket** broadcast at 20 Hz
- **Static Three.js dashboard** at `/` — no build step, no npm. The
  importmap pulls `three@0.165.0` from unpkg directly
- **6 background tasks** launched in the FastAPI lifespan context
- **Event hooks** wire `firmware event → emotion` and
  `action dispatch → autonomy + emotion` in `main.py::_setup_hooks()`

### Connection modes

Set env vars before running `start.bat`:

| Env var                  | Effect |
|--------------------------|--------|
| (default)                | USB serial on `ROBOT_PORT` (default `COM3`) |
| `ROBOT_TCP_SERVER=1`     | Listen on port 9000 for the ESP to connect inward (WiFi mode) |
| `ROBOT_WIFI_HOST=192.168.x.x` | Brain connects outward to the ESP (legacy) |

### Background tasks

| Task                      | Job |
|---------------------------|-----|
| `robot_connection_task`   | maintains serial/TCP link, reconnects on drop |
| `state_broadcast_task`    | pushes state JSON to all dashboard websockets at 20 Hz |
| `watchdog_task`           | flags the robot disconnected after 3 s of silence; serial watchdog force-closes after 8 s |
| `ws_keepalive_task`       | pings browsers every 20 s to defeat proxy/browser idle kills |
| `emotion_update_task`     | decays emotion toward the time-of-day baseline every 1 s |
| `autonomy_engine.run()`   | spontaneous behavior tick loop (every 5 s) + a separate fidget micro-loop |

### REST API

| Method | Path | Description |
|--------|------|-------------|
| GET  | `/`                                | Dashboard HTML |
| WS   | `/ws`                              | 20 Hz state broadcast + command channel |
| GET  | `/api/state`                       | Combined robot + emotion state |
| GET  | `/api/emotion`                     | Current emotion (valence, arousal, label) |
| POST | `/api/emotion/stimulate/{event}`   | Fire a stimulus manually (testing) |
| GET  | `/api/ports`                       | List available serial ports |
| POST | `/api/cmd/walk/{F\|B\|L\|R}`       | Crawl gait |
| POST | `/api/cmd/trot/{F\|B}`             | Trot gait (faster) |
| POST | `/api/cmd/stop`                    | Emergency stop |
| POST | `/api/cmd/behavior/{state}`        | idle / alert / happy / curious / sleepy / sleep |
| POST | `/api/cmd/body`                    | Body translation `{x,y,z}` (mm) |
| POST | `/api/cmd/twist`                   | Combined move+rotate `{mx,my,mz,rx,ry,rz}` |
| POST | `/api/cmd/speed/{1-10}`            | Animation/gait speed scalar |
| POST | `/api/cmd/sleep` / `/api/cmd/wake` | Sleep / wake |
| POST | `/api/cmd/diag`                    | Request diagnostic report (SRAM, uptime) |
| POST | `/api/llm/act`                     | Dispatch a named LLM action `{"action":"greet","params":{}}` |
| GET  | `/api/llm/tools`                   | Anthropic tool_use schema for the 21 actions |
| GET  | `/api/llm/prompt`                  | System prompt for LLM sessions |
| POST | `/api/calibrate/prep`              | Enter belly-down calibration pose |
| POST | `/api/calibrate/nudge`             | Move a leg tip by Δ(x,y,z) in mm |
| POST | `/api/calibrate/save`              | Persist offsets to EEPROM |
| POST | `/api/calibrate/check`             | Verify alignment |
| POST | `/api/calibrate/exit`              | Return to idle |
| POST | `/api/autonomy/enable`             | Flip autonomy on/off at runtime |

---

## Serial protocol

Brain ↔ robot uses line-terminated ASCII commands one way and
newline-delimited JSON the other.

### Commands (brain → robot, newline terminated)

| Command   | Format                                               | Notes |
|-----------|------------------------------------------------------|-------|
| `PING`    | `PING`                                               | Robot replies with `{"t":"pong"}` |
| `STATUS`  | `STATUS`                                             | Robot sends an immediate state report |
| `STOP`    | `STOP`                                               | Set state to IDLE |
| `SLEEP`   | `SLEEP`                                              | Enter SLEEP state |
| `WAKE`    | `WAKE`                                               | Return to IDLE from sleep |
| `WALK`    | `WALK F\|B\|L\|R`                                   | Crawl gait (slower, more stable) |
| `TROT`    | `TROT F\|B`                                         | Trot gait (faster, diagonal pairs) |
| `BEHAV`   | `BEHAV idle\|alert\|happy\|curious\|sleepy\|sleep\|excited` | Set animation state |
| `BODY`    | `BODY <x> <y> <z>`                                   | Body translation in mm, integers, clamped ±30 / ±20 |
| `ROTATE`  | `ROTATE <x> <y> <z>`                                 | Body rotation in degrees × 10 (e.g. `100` = 10.0°) |
| `TWIST`   | `TWIST <mx> <my> <mz> <rx> <ry> <rz>`                | Combined move+rotate, all × 10 integers |
| `SPEED`   | `SPEED <1-10>`                                       | Animation/gait speed (5 = normal) |
| `BUZZ`    | `BUZZ <pattern>`                                     | Play emotion sound (see patterns below) |
| `DIAG`    | `DIAG`                                               | Request diagnostic report (SRAM, uptime) |
| `CALI`    | `CALI PREP\|NUDGE\|SAVE\|CHECK\|RESET\|EXIT ...`    | Calibration commands |

**BUZZ patterns:** `boot`, `happy`, `excited`, `curious`, `alert`, `sleepy`, `shy`, `greet`, `ack`

### Reports (robot → brain, JSON lines)

| Type   | Format                                                     | When sent |
|--------|------------------------------------------------------------|-----------|
| State  | `{"t":"s","v":<v×100>,"s":"<state>","b":<0\|1>}`          | Every 50 ms idle, 150 ms walking/trot |
| Pong   | `{"t":"pong"}`                                             | In response to `PING` |
| Diag   | `{"t":"diag","sram":<bytes>,"up":<seconds>}`               | In response to `DIAG` |
| Event  | `{"t":"evt","e":"boot"}`                                   | Once on first loop tick |
| Event  | `{"t":"evt","e":"low_bat"}`                                | Once when voltage crosses 7.0 V downward |

Voltage is encoded as `int = V × 100` (e.g. `734` = `7.34 V`) to avoid
floating-point in the JSON serializer.

---

## Emotion engine

Spider's internal state is modelled on the **circumplex model of affect**
— two continuous axes:

- **Valence** (−1.0 distressed ↔ +1.0 pleased)
- **Arousal** (−1.0 dormant ↔ +1.0 excited)

Both axes decay exponentially toward a **time-of-day baseline** every
second. Stimuli push the state instantly; the decay creates emotional
inertia, so Spider stays happy for a while after an interaction and
doesn't snap between states.

### Quadrant → label mapping

```
                          AROUSAL high
                 alert    │    excited / happy
       VALENCE neg  ──────┼──────  VALENCE pos
                 sleepy   │    curious / idle
                          AROUSAL low
```

### Stimuli

Each stimulus is a `(dValence, dArousal)` push. A sampling:

| Event             | dValence | dArousal |
|-------------------|----------|----------|
| `person_seen`     | +0.40    | +0.30    |
| `command_received`| +0.20    | +0.15    |
| `celebration`     | +0.65    | +0.70    |
| `low_battery`     | −0.35    | −0.20    |
| `alone_long_time` | −0.10    | −0.10    |
| `startled`        | −0.10    | +0.60    |

Full list: `person_seen`, `face_recognized`, `touched`, `command_received`,
`play_started`, `play_finished`, `celebration`, `picked_up`, `put_down`,
`loud_noise`, `startled`, `alone_long_time`, `low_battery`, `battery_ok`,
`object_seen`, `new_environment`.

### Time-of-day baselines

| Hours        | Valence | Arousal | Flavor          |
|--------------|---------|---------|-----------------|
| 06:00–10:00  | +0.20   | +0.15   | bright, energetic |
| 10:00–18:00  | +0.15   |  0.00   | content, neutral |
| 18:00–22:00  | +0.10   | −0.10   | calm, warm      |
| 22:00–00:00  |  0.00   | −0.40   | sleepy          |
| 00:00–06:00  | −0.05   | −0.55   | very sleepy     |

---

## Autonomy engine

Background asyncio task that generates **spontaneous behavior** when no
human is actively controlling Spider.

### Parameters

| Constant           | Default | Meaning |
|--------------------|---------|---------|
| `_TICK_INTERVAL`   | 5 s     | How often the tick loop considers acting |
| `_COMMAND_LOCKOUT` | 10 s    | Seconds after any human/LLM command before autonomy resumes |
| `_ACTION_PROB`     | 0.90    | Probability of picking an action per tick |

### Tick flow

1. Bail out if disabled, disconnected, busy, mid-calibration, inside
   lockout window, or in SLEEP state
2. Bail out if battery is critically low (< 5.5 V)
3. Decay emotion toward the time-of-day baseline
4. If alone > 120 s, apply `alone_long_time` stimulus (scale 0.3)
5. Sync firmware `BEHAV` state to match the current emotion label
6. With `_ACTION_PROB` probability, call `_act()`
7. `_act()` tries the LLM path, falls back to the rule-based path

### LLM path — Ollama

Posts a chat-completion request to `http://localhost:11434/api/chat`
(override with `OLLAMA_BASE_URL` env var) with:

- **System prompt** — a terse "You are Spider, a four-legged creature..."
  character sheet with hard rules against repetition, forced alternation
  between locomotion and expression, and explicit battery-safety caveats
- **User message** — perception context (time of day, emotion state,
  last 5 actions, camera observations if available) + compact action menu
- **Response format** — forced JSON:
  `{"thought": "5-10 word impulse", "action": "name", "params": {...}}`

Parsed strictly first, then loosely (scan for any known action name in
the text) for models that wrap JSON in markdown fences. If the picked
action isn't in `_LOW_BATTERY_SAFE` and battery < 6.5 V, it's overridden
to `wait(2 s)` before dispatch.

Default model: `llama3.2:3b` (override with `OLLAMA_MODEL` env var).
Recommended alternatives:

| Model        | RAM    | Latency | Quality |
|--------------|--------|---------|---------|
| `llama3.2:3b`| ~2 GB  | ~0.5 s  | snappy, good enough |
| `llama3.1:8b`| ~5 GB  | ~2 s    | smarter choices |
| `gemma3:12b` | ~7 GB  | ~4 s    | very good reasoning |

### Rule-based fallback

If Ollama is unreachable or returns bad output, the engine falls back to
a **random weighted pick from a per-emotion action palette**. Spider
still behaves, just less contextually. The palettes live in
`autonomy.py::_pick_action_rule_based()` and are scoped per emotion
label (`sleepy`, `idle`, `curious`, `happy`, `excited`, `alert`).

### Fidget loop

A second `_fidget_loop()` coroutine runs independently and fires small
micro-animations (head tilts, peeks, recenters, curious chirps) during
the **lockout window** — the 10-second gap after a command — so Spider
stays visibly alive while the LLM is thinking or the user has just let go
of the controls. The fidget loop won't fire during locomotion, sleep, or
calibration.

---

## LLM action vocabulary

21 named actions, each returns a description string. All dispatched
through `actions.py::dispatch(name, params)` with a uniform
post-dispatch hook that informs the autonomy lockout and the emotion
engine.

| Category      | Actions |
|---------------|---------|
| Locomotion    | `walk_forward`, `walk_backward`, `turn_left`, `turn_right`, `stop` |
| Expression    | `set_mood`, `greet`, `celebrate`, `investigate`, `shy`, `frightened`, `alert` |
| Body language | `bow`, `look_at`, `center`, `stretch`, `crouch`, `tilt_head` |
| Sound         | `chirp` |
| Meta          | `wait`, `get_state` |

- `TOOLS` — Anthropic tool_use schema list (paste directly into
  `tools=` in an API call)
- `SYSTEM_PROMPT` — Spider personality + rules string (inject as the
  system message of a chat session)

Both are served as JSON at `GET /api/llm/tools` and `GET /api/llm/prompt`
respectively, so an external LLM session can bootstrap itself from the
running brain server.

---

## Design decisions (and why)

Hardware constraints and implementation tradeoffs.

- **FNQRBasic is kept verbatim.** The IK math is float-based, tested, and
  correct. Rewriting it introduces risk with no benefit; the performance
  cost of the float math is irrelevant next to the 20 ms servo tick rate.

- **No NRF24L01 code anywhere.** We stripped the radio entirely. The
  project does not want remote control; it wants autonomy. The SPI bus
  is free for future use.

- **PROGMEM sine table instead of `sinf()`.** 64 entries, `int8_t`,
  ~2 µs per evaluation vs ~200 µs on AVR. The animation engine is called
  every 20 ms across 12 joints — the savings matter.

- **No `malloc` anywhere in firmware.** All buffers are stack or static.
  AVR heap fragmentation is fatal in long-running code and impossible
  to debug without a JTAG rig.

- **No `sprintf` or `sscanf`.** Both cost 1–2 KB of flash and 1–2 ms per
  call on AVR. Replaced with a manual inline parser (`nextInt`) and a
  manual JSON builder (`writeInt` + `memcpy_P`).

- **Yield-callback pattern for long gait calls.** The Freenove IK engine
  uses blocking `WaitUntilFree()` loops. `BrainComm` registers
  `brainCommYield()` as the yield callback, so serial reads and buzzer
  ticks happen during the blocking sections. Without this, commands
  drop during walking.

- **Polling buzzer driver — no ISR.** A Timer4 CTC ISR for tone
  generation caused `multiple definition of '__vector_42'` at link time,
  because the Servo library claims every usable timer vector at compile
  time — even the ones we don't need. Polling from `update()` works
  because the main loop ticks faster than even a 5 kHz tone needs.

- **Trot gait uses absolute target positions.** Relative offsets
  accumulate drift across cycles. Absolute `MoveTo()` targets make the
  2-phase (leg1+leg4 / leg2+leg3) cycle perfectly repeatable across
  arbitrarily long trot runs.

- **Circumplex emotion model (not discrete states).** Two continuous
  axes give the emotion natural inertia and smooth blending. Spider
  stays happy for a while after an interaction; it doesn't snap
  instantly between discrete moods.

- **10-second autonomy lockout.** Long enough that human commands feel
  authoritative (nothing worse than your pet overriding your inputs);
  short enough that Spider resumes its own life quickly when ignored.

- **Brain is the smart layer, firmware is the actuator.** The ATmega has
  no FPU and 8 KB of SRAM. Running any LLM-flavored code on it is
  hopeless. The brain (desktop, 16 GB RAM) runs the model, the emotion
  engine, the perception pipeline, and all decision making. The ATmega
  does nothing but servo interpolation and a fast command protocol.

---

# ollamaped

> An open-source, fully-local vision+LLM-driven autonomous quadruped pet.
> No cloud, no API keys, no teleop. The robot picks its own actions from a local model.

Built on the **Freenove Quadruped Robot Kit V2** (ATmega2560 + 12 servos),
driven by a **Python / FastAPI brain server** on a LAN-connected desktop
that feeds perception context to **Ollama** and turns the model's responses
into motion, sound, and body language.

<!-- Replace the paths below with real files once you drop photos into docs/images/ -->
<p align="center">
  <img src="docs/images/hero.jpg" alt="ollamaped on its calibration sheet" width="600">
  <br><em>(Photo placeholder — drop a hero image at <code>docs/images/hero.jpg</code>.)</em>
</p>

The robot idles, sways, wakes up, gets curious, investigates, gets bored,
naps, complains about low battery, and picks its own actions — all from a
local model running on the same LAN. The original Freenove Processing app
and stock sketch firmware have been fully replaced.

**Status:** working prototype under active development. Not a finished
product — see [Current status & roadmap](#current-status--roadmap) for what
is real, what is stubbed, and what is still blocked on parts.

---

## Table of contents

- [Highlights](#highlights)
- [Hardware](#hardware)
- [Architecture](#architecture)
- [Repo layout](#repo-layout)
- [Firmware deep dive](#firmware-deep-dive)
- [Brain server deep dive](#brain-server-deep-dive)
- [Serial protocol](#serial-protocol)
- [Emotion engine](#emotion-engine)
- [Autonomy engine](#autonomy-engine)
- [LLM action vocabulary](#llm-action-vocabulary)
- [Quickstart](#quickstart)
- [WiFi untethering](#wifi-untethering)
- [Current status & roadmap](#current-status--roadmap)
- [Design decisions (and why)](#design-decisions-and-why)
- [Attribution & license](#attribution--license)

---

## Highlights

- **100% local intelligence** — Ollama with `llama3.2:3b` by default, no
  cloud calls, no API keys, no telemetry
- **Circumplex emotion model** — two continuous axes (valence / arousal)
  with exponential decay toward a time-of-day baseline, driven by stimuli
- **14-state firmware animation engine** — breathing, alert, happy,
  curious, sleepy, walking, turning, trot, excited, calibrate
- **Non-blocking polling buzzer** — 9 emotion patterns, no timer ISR
  (the Servo library claims all the usable timer vectors)
- **Full Freenove replacement** — the original Processing app and stock
  sketch firmware are gone. This is a clean rewrite in C++ and Python
  with a FastAPI + WebSocket dashboard
- **Live 3D viewport** — Three.js via CDN importmap, no build step, no npm
- **Browser-based servo calibration** — Freenove belly-down flow
  (belly down on a printed template, nudge each leg tip onto its cross,
  save EEPROM offsets) in the dashboard, no serial console required
- **Two gaits** — stable crawl (one leg at a time) and ~2× faster trot
  (diagonal pairs via absolute-position 2-phase cycle)
- **21-action LLM vocabulary** — exposed as an Anthropic tool_use schema
  at `GET /api/llm/tools`, dispatched uniformly whether the caller is
  autonomy, a human via REST, or the dashboard

---

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
│       ├── vision/              ← vision pipeline (stubbed, Ollama moondream backend)
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

## Quickstart

### Prerequisites

- **Freenove Quadruped Robot Kit V2** (or clone — same BOM works)
- **Arduino CLI** or Arduino IDE with the AVR board package
- **Python 3.10+** on the brain machine (Windows / Linux / macOS)
- **Ollama** installed on the brain machine (https://ollama.com)
- Both the robot and the brain on the same LAN (only needed for WiFi mode —
  USB-only works fine on a single machine)

### 1. Clone and set up secrets

```bash
git clone https://github.com/all3n1k/ollamaped.git
cd ollamaped
```

Each firmware sketch has its own `secrets.h` / `Secrets.h` file which is
gitignored. Before building, copy the `.example.h` templates to the real
filename and fill in your values:

```bash
cp firmware/spider_firmware/Secrets.example.h firmware/spider_firmware/Secrets.h
cp firmware/spider_esp/secrets.example.h      firmware/spider_esp/secrets.h
cp tools/esp32cam_firmware/secrets.example.h  tools/esp32cam_firmware/secrets.h
# (etc. — copy any sketch you plan to build)
```

Edit each `Secrets.h` / `secrets.h` with your WiFi SSID, WiFi password,
brain server LAN IP, and OTA password (if using OTA).

### 2. Flash the main firmware

```bash
cd firmware/spider_firmware
arduino-cli compile --fqbn arduino:avr:mega .
arduino-cli upload  --fqbn arduino:avr:mega -p COM3 .
```

(Replace `COM3` with whatever port your ATmega shows up on.)

You should hear the boot chirp. Over serial at 115200 baud you should
see `{"t":"evt","e":"boot"}` and then state reports at ~50 ms cadence.

### 3. Set up the brain server

```bash
cd ../../brain
python -m venv .venv
.venv\Scripts\activate          # Windows
# source .venv/bin/activate     # Linux / macOS
pip install -r requirements.txt
```

### 4. Install Ollama and pull a model

Install Ollama from https://ollama.com, then:

```bash
ollama pull llama3.2:3b
```

Verify Ollama is running:

```bash
curl http://localhost:11434/api/tags
```

### 5. Run the brain

```bash
# Windows:
start.bat

# or directly:
python src/main.py
```

The brain will open port 8080 for the dashboard and attempt to open
`COM3` (or whatever `ROBOT_PORT` is set to) for the robot.

### 6. Open the dashboard

Browse to `http://localhost:8080` on any machine on the LAN. You should
see the 3D viewport, live state, emotion label, battery bar, and the
behavior/movement/calibration panels.

### 7. Calibrate

Click the `Servo Calibration` panel at the bottom, print the cal
template via the link, place the robot belly-down on the page, and
follow the in-panel instructions. Save offsets when each leg tip sits
on its cross.

---

## WiFi untethering

Two paths, both supported:

1. **Stock AT firmware** — leave the ESP-01S running its factory AT
   firmware and let the ATmega configure it via `AT+` commands. Set
   `WIFI_ENABLED 1` in `Config.h`, rebuild and reflash the ATmega. No
   ESP flash required. Works today; latency is higher due to AT overhead.

2. **Custom ESP-01S firmware** — flash `firmware/spider_esp/spider_esp.ino`
   onto the ESP-01S. This gives you a transparent Serial ↔ TCP bridge
   plus ArduinoOTA so subsequent updates go over the air. Requires a
   one-time physical flash via USB programmer dongle because GPIO0 of
   the ESP-01S is hardwired to 3.3V on the V2 board — software
   bootloader mode is physically impossible on this hardware.

   After the first flash, start the brain with `ROBOT_TCP_SERVER=1` so
   it listens on port 9000 for the ESP to connect inward.

---

## Current status & roadmap

### Working

- [x] Full firmware rewrite — 14 states, IK, crawl + trot, buzzer
- [x] Brain server — FastAPI, WebSocket, 6 background tasks
- [x] Dashboard — live 3D viewport, behavior/movement/body panels
- [x] Emotion engine — circumplex, stimuli, time-of-day baselines
- [x] Autonomy engine — Ollama LLM path + rule-based fallback
- [x] 21-action LLM vocabulary with Anthropic tool_use schema endpoint
- [x] Browser-based servo calibration (belly-down nudge flow)
- [x] Low-battery event routing (`low_bat` → `low_battery` stimulus)
- [x] Adaptive state report rate (50 ms idle, 150 ms walking)

### Known issues / actively debugging

- [ ] **Movement commands are sometimes dropped** when the robot is mid-routine.
      Suspect a busy-state race between the yield callback and blocking
      `WaitUntilFree()` calls inside the IK engine
- [ ] **ESP-01S occasional disconnects** during long sessions. Probably
      reconnection timing, not signal — need to add RSSI/retry telemetry
- [ ] **Servo calibration entry is flaky** — `Enter Cal Mode` sometimes
      no-ops and EEPROM offsets occasionally don't persist across power
      cycles. Current hypothesis is that `CalibrateState()` + blocking
      `WaitUntilFree()` can silently fail on certain startup postures

### Blocked on hardware

- [ ] ESP-01S custom firmware flashed — needs USB programmer dongle (ordered)
- [ ] ATmega OTA via ESP-01S — depends on above
- [ ] ESP32-CAM wiring — hardware ordered, not yet wired
- [ ] IMU integration — MPU-6050 on P3 I2C, would enable tap/tilt + auto-leveling
- [ ] LED eyes — visual emotion expression
- [ ] Microphone — Whisper voice commands + `loud_noise` stimulus
- [ ] Cliff / edge sensors — IR or ToF on P3 A1

### Not started

- [ ] Vision pipeline — `brain/src/vision/__init__.py` has a full
      implementation guide and an Ollama `moondream` / `llava-phi3`
      backend sketch. Set `VISION_ENABLED=True` once the camera arrives
- [ ] Dashboard photo gallery + video loop for this README

---

## Design decisions (and why)

These are the unobvious choices — the ones that would get second-guessed
in a code review but are correct for this hardware.

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

## Attribution & license

This project contains code adapted from the **Freenove Quadruped Robot
Kit V2** official examples, specifically `FNQRBasic.h` / `FNQRBasic.cpp`
(the IK engine and `RobotAction` API), which are kept verbatim from the
Freenove reference. See https://github.com/Freenove/Freenove_Quadruped_Robot_Kit_V2
for the upstream source. All other firmware and brain-server code is
new and written for this project.

**License:** pending. The repository as published is source-available
for reference, learning, and hacking. Before redistribution or
derivative work please check Freenove's upstream license terms for the
retained IK engine files. A formal LICENSE file will be added once
that reconciliation is done.

---

## Contributing

This is a hobby project that's actively being hacked on — issues and PRs
are welcome, but expect sharp edges. If you're thinking of building
something similar, the [Design decisions](#design-decisions-and-why)
section above is probably the most useful starting point: it explains
the things that aren't obvious from the code alone.

If you get it running on a different board (Arduino Mega clone, different
servo count, different WiFi module), please open an issue with notes —
compatibility reports are genuinely helpful.

# Ollamaped

Firmware and a desktop control server for the Freenove Quadruped Robot Kit V2.
The ATmega2560 handles servo motion, gaits, and sound. A Python/FastAPI server
handles state, browser controls, perception, and optional Ollama-driven actions.

This is a working prototype with hardware-dependent limitations. It supports
USB serial and ESP8266 connections; the dashboard provides movement, behavior,
and servo-calibration controls.

## Hardware and software

- Freenove Quadruped Robot Kit V2: ATmega2560 and 12 servos.
- Optional ESP-01S Wi-Fi bridge and ESP32-CAM.
- Arduino IDE or Arduino CLI with the AVR board package.
- Python 3.10+ on the desktop.
- Ollama for local autonomous decisions. Optional vision backends are configured
  separately; the code also includes an Anthropic vision integration.

## Firmware setup

```sh
git clone https://github.com/all3n1k/ollamaped.git
cd ollamaped
cp firmware/spider_firmware/Secrets.example.h firmware/spider_firmware/Secrets.h
```

Fill in your own settings in the ignored secrets file. Other sketches have
their own `secrets.example.h` templates.

To build and upload the main firmware:

```sh
arduino-cli compile --fqbn arduino:avr:mega firmware/spider_firmware
arduino-cli upload --fqbn arduino:avr:mega -p COM3 firmware/spider_firmware
```

Replace `COM3` with your board's port. Use the dashboard calibration flow
before testing movement, and keep the robot supported during initial checks.

## Desktop server

```sh
cd brain
python -m venv .venv
```

Activate the environment with `source .venv/bin/activate` on macOS/Linux or
`.venv\Scripts\activate` on Windows, then:

```sh
python -m pip install -r requirements.txt
```

For USB startup without autonomous movement, set the following environment
variables before running `python src/main.py`:

| Variable | Value |
| --- | --- |
| `ROBOT_PORT` | Your serial port, for example `COM3` |
| `ROBOT_TCP_SERVER` | Defaults to `0` (USB); `1` opts into incoming ESP8266 TCP |
| `ROBOT_TCP_BIND`, `ROBOT_TCP_PEER` | Explicit brain interface IP and ESP8266 IP for incoming TCP |
| `AUTONOMY_AUTOSTART` | `0` to leave autonomous decisions paused |

Open http://localhost:8080 for the dashboard. An incoming TCP connection on port 9000 requires explicit opt-in and both IPs. Use a trusted, isolated device network; the firmware transport is not encrypted.

For local model-driven operation, start Ollama, install a model, and set
`OLLAMA_MODEL` to its exact installed name. `OLLAMA_URL` defaults to
`http://localhost:11434`. Enable autonomy after confirming the connection,
calibration, and available movement space.

## Implementation

The firmware includes crawl/trot motion, a behavior state machine, a polling
buzzer driver, and a serial command protocol. The desktop provides a shared
robot state, emotion/autonomy loops, action dispatch, WebSocket updates, and a
Three.js viewport.

- [Architecture and protocol reference](docs/architecture.md)
- [Development and hardware guides](docs/how-to-guides.md)
- [Runtime action schema](brain/src/robot/actions.py)
- [Firmware configuration](firmware/spider_firmware/Config.h)

## Limitations

Movement dispatch, calibration persistence, and Wi-Fi reconnection have known
hardware-dependent issues. Vision support in source does not establish that
a particular camera installation has been tested. Current hardware behavior
must be checked on the actual robot; there are no performance or reliability
guarantees here.

## Attribution and license

The project retains Freenove's `FNQRBasic.h` / `FNQRBasic.cpp` IK engine and
`RobotAction` API. See the
[upstream Freenove examples](https://github.com/Freenove/Freenove_Quadruped_Robot_Kit_V2).

A repository-wide license is pending reconciliation of the retained upstream
code. The published source remains available for reference and experimentation;
check the applicable upstream terms before redistribution.

## Contributing

Issues and pull requests are welcome. Include the board revision, connection
mode, reproduction steps, and logs for hardware reports.

## Dashboard access

The dashboard binds to loopback by default. For access through a private remote
interface, explicitly set `DASHBOARD_HOST`, `DASHBOARD_ALLOWED_HOSTS` (comma-separated
request hostnames), and a random `DASHBOARD_TOKEN` of at least 24 characters.
The browser asks for HTTP Basic credentials; use any username and the token as
the password. API clients may instead send `Authorization: Bearer <token>`.
Use a trusted TLS tunnel for remote access. Cross-origin browser requests and
unrecognized Host headers are rejected, including WebSocket handshakes.
Keep credentials in local environment/configuration, outside source control.

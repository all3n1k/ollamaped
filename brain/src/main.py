"""
Spider Robot Brain Server
FastAPI + WebSocket — replaces the Processing app entirely.
Serves the dashboard UI and bridges commands to the robot.
"""
import asyncio
import json
import logging
import os
import time
from contextlib import asynccontextmanager
from typing import Set

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from robot.comm     import robot_comm
from robot.state    import robot_state, set_event_callback
from robot          import actions  as robot_actions
from robot.emotion  import emotion_engine
from robot.autonomy import autonomy_engine, _COMMAND_LOCKOUT

# ---------------------------------------------------------------------------
# Config — edit these or override with env vars
# ---------------------------------------------------------------------------
SERIAL_PORT  = os.getenv("ROBOT_PORT",  "COM3")
SERIAL_BAUD  = int(os.getenv("ROBOT_BAUD", "115200"))
WIFI_HOST    = os.getenv("ROBOT_WIFI_HOST", "")   # set to ESP8266 IP to use WiFi (outbound)
TCP_SERVER   = os.getenv("ROBOT_TCP_SERVER", "1") == "1"  # listen for ESP8266 connecting in
TCP_PORT     = int(os.getenv("ROBOT_TCP_PORT", "9000"))
SERVER_HOST  = os.getenv("SERVER_HOST", "0.0.0.0")
SERVER_PORT  = int(os.getenv("SERVER_PORT", "8080"))
AUTONOMY_AUTOSTART = os.getenv("AUTONOMY_AUTOSTART", "1") == "1"  # set to 0 to hold still on boot
UI_BROADCAST_HZ = 20   # state pushed to all browser clients at this rate

logging.basicConfig(level=logging.INFO, format="%(levelname)s  %(name)s  %(message)s")
log = logging.getLogger("brain")

# ---------------------------------------------------------------------------
# WebSocket connection manager — broadcasts state to all connected browsers
# ---------------------------------------------------------------------------
class ConnectionManager:
    def __init__(self):
        self._clients: Set[WebSocket] = set()

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self._clients.add(ws)
        log.info("UI client connected (%d total)", len(self._clients))

    def disconnect(self, ws: WebSocket):
        self._clients.discard(ws)
        log.info("UI client disconnected (%d total)", len(self._clients))

    async def broadcast(self, data: dict):
        if not self._clients:
            return
        msg = json.dumps(data)
        dead = set()
        for ws in self._clients:
            try:
                await ws.send_text(msg)
            except Exception:
                dead.add(ws)
        self._clients -= dead

    async def brain_log(self, tag: str, msg: str, level: str = "info"):
        """Send a brain console entry to all UI clients."""
        await self.broadcast({"type": "brain_log", "level": level, "tag": tag, "msg": msg})


manager = ConnectionManager()

# ---------------------------------------------------------------------------
# Background tasks
# ---------------------------------------------------------------------------
async def robot_connection_task():
    """Maintain robot connection.
    Modes (set via env vars):
      ROBOT_WIFI_HOST=<ip>    — brain connects out to ESP8266 (legacy AP mode)
      ROBOT_TCP_SERVER=1      — brain listens, ESP8266 connects in (new firmware)
      (default)               — USB serial on ROBOT_PORT
    """
    if WIFI_HOST:
        log.info("WiFi outbound mode → %s", WIFI_HOST)
        await robot_comm.run_wifi(WIFI_HOST)
    elif TCP_SERVER:
        log.info("TCP server mode — waiting for ESP8266 on port %d", TCP_PORT)
        await robot_comm.run_tcp_server(TCP_PORT)
    else:
        log.info("Serial mode on %s @ %d", SERIAL_PORT, SERIAL_BAUD)
        await robot_comm.run_serial(SERIAL_PORT, SERIAL_BAUD)


async def state_broadcast_task():
    """Push robot state to all UI clients at UI_BROADCAST_HZ."""
    interval = 1.0 / UI_BROADCAST_HZ
    while True:
        await asyncio.sleep(interval)
        await manager.broadcast({"type": "state", "data": robot_state.to_dict()})


async def watchdog_task():
    """Mark robot disconnected if no report received in 3 seconds."""
    while True:
        await asyncio.sleep(1.0)
        if robot_state.connected and (time.time() - robot_state.last_seen) > 3.0:
            robot_state.connected = False
            robot_state.add_event("Watchdog: no report for 3s")


async def emotion_update_task():
    """Decay emotion toward baseline every second."""
    while True:
        await asyncio.sleep(1.0)
        emotion_engine.update()


async def ws_keepalive_task():
    """Send WebSocket ping to all UI clients every 20s.
    Prevents browsers and proxies from killing idle connections."""
    while True:
        await asyncio.sleep(20.0)
        dead = set()
        for ws in list(manager._clients):
            try:
                await ws.send_text('{"type":"ping"}')
            except Exception:
                dead.add(ws)
        manager._clients -= dead


# ---------------------------------------------------------------------------
# App lifespan
# ---------------------------------------------------------------------------
async def _boot_scan():
    """
    Environment scan run once after firmware boot event.
    Rotates 3× taking camera snapshots, logs what Spider sees, then settles to idle.
    """
    await asyncio.sleep(2.0)   # let firmware stabilize
    await manager.brain_log("boot", "Boot sequence starting — scanning environment", level="info")
    await robot_comm.set_behavior("alert")
    await asyncio.sleep(0.8)

    from vision import get_scene_description
    for i in range(3):
        await robot_comm.walk("R")
        await asyncio.sleep(1.0)
        await robot_comm.stop()
        await asyncio.sleep(0.4)
        desc = await get_scene_description()
        if desc:
            await manager.brain_log("boot", f"Scan {i+1}/3: {desc}", level="info")
        else:
            await manager.brain_log("boot", f"Scan {i+1}/3: (no camera data)", level="info")
        await asyncio.sleep(0.3)

    await robot_comm.set_behavior("idle")
    if AUTONOMY_AUTOSTART:
        autonomy_engine._last_commanded_at = 0.0
        await manager.brain_log("boot", "Boot complete — Spider is wandering", level="info")
    else:
        await manager.brain_log("boot", "Boot complete — autonomy DISABLED (send POST /api/autonomy/enable to start)", level="info")


def _setup_hooks():
    """Wire emotion + autonomy engines into the action/event pipelines."""

    # Firmware event → emotion stimulus
    def _on_robot_event(evt: str):
        if evt == "low_bat":
            emotion_engine.stimulate("low_battery")
            robot_state.add_event("Low battery — Spider is unhappy")
            asyncio.create_task(robot_comm.buzz("warning"))
        elif evt == "boot":
            emotion_engine.stimulate("battery_ok", scale=0.5)
            asyncio.create_task(robot_comm.buzz("boot"))
            asyncio.create_task(_boot_scan())

    set_event_callback(_on_robot_event)

    # Action dispatch → reset autonomy lockout + stimulate emotion + brain log
    _PLAY_ACTIONS = {"greet", "walk_forward", "walk_backward",
                     "turn_left", "turn_right", "investigate", "tilt_head", "bow"}
    _STRESS_ACTIONS = {"shy", "frightened", "alert"}

    def _on_action(name: str):
        autonomy_engine.on_command_received()
        if name in _PLAY_ACTIONS:
            emotion_engine.stimulate("command_received")
        elif name == "celebrate":
            emotion_engine.stimulate("celebration")
        elif name in _STRESS_ACTIONS:
            emotion_engine.stimulate("startled", scale=0.4)
        asyncio.create_task(manager.brain_log("cmd", f"→ {name}", level="cmd"))

    robot_actions.set_post_dispatch_hook(_on_action)

    # Autonomy engine → brain log
    def _on_autonomy(action: str, reason: str, source: str):
        asyncio.create_task(manager.brain_log(
            "autonomy", f"[{source}] {action}  ← {reason}", level="autonomy"
        ))

    autonomy_engine.set_log_hook(_on_autonomy)

    # Robot events → brain log
    def _on_robot_event_log(evt: str):
        asyncio.create_task(manager.brain_log("robot", f"event: {evt}", level="robot"))

    set_event_callback(_on_robot_event_log, secondary=True)


async def rssi_poll_task():
    """Poll ESP32-CAM for WiFi RSSI scan every 30s and feed into pose tracker."""
    import httpx
    while True:
        await asyncio.sleep(30.0)
        try:
            async with httpx.AsyncClient(timeout=5.0) as client:
                resp = await client.get("http://192.168.1.210/rssi_scan")
            if resp.status_code == 200:
                scan_data = resp.json()
                networks = {n["ssid"]: n["rssi"] for n in scan_data.get("networks", []) if n.get("ssid")}
                if networks:
                    from robot.pose import pose_tracker
                    pose_tracker.update_from_rssi(networks)
                    log.debug("RSSI scan: %d networks", len(networks))
        except Exception:
            pass  # silently skip if camera is down or endpoint not yet flashed


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Load apartment map for localization
    from robot.pose import pose_tracker
    _map_path = os.path.join(os.path.dirname(__file__), "robot", "apartment_map.json")
    pose_tracker.load_map(_map_path)

    # Autonomy starts disabled unless AUTONOMY_AUTOSTART=1
    autonomy_engine.set_enabled(AUTONOMY_AUTOSTART)
    if not AUTONOMY_AUTOSTART:
        log.info("Autonomy disabled at boot — POST /api/autonomy/enable to start")

    _setup_hooks()
    from robot.perception import vision_loop
    tasks = [
        asyncio.create_task(robot_connection_task()),
        asyncio.create_task(state_broadcast_task()),
        asyncio.create_task(watchdog_task()),
        asyncio.create_task(ws_keepalive_task()),
        asyncio.create_task(emotion_update_task()),
        asyncio.create_task(autonomy_engine.run()),
        asyncio.create_task(rssi_poll_task()),
        asyncio.create_task(vision_loop()),
    ]
    yield
    for t in tasks:
        t.cancel()


app = FastAPI(title="Spider Robot Brain", lifespan=lifespan)

# Serve static UI files
_ui_path = os.path.join(os.path.dirname(__file__), "ui", "static")
app.mount("/static", StaticFiles(directory=_ui_path), name="static")

# ---------------------------------------------------------------------------
# HTTP routes
# ---------------------------------------------------------------------------
@app.get("/", response_class=HTMLResponse)
async def dashboard():
    index = os.path.join(_ui_path, "..", "index.html")
    return FileResponse(index)


@app.get("/api/state")
async def get_state():
    return {**robot_state.to_dict(), "emotion": emotion_engine.to_dict()}


@app.get("/api/emotion")
async def get_emotion():
    return emotion_engine.to_dict()


@app.post("/api/emotion/stimulate/{event}")
async def stimulate_emotion(event: str, scale: float = 1.0):
    """Manually fire an emotion stimulus. Useful for testing."""
    from robot.emotion import STIMULI
    if event not in STIMULI:
        return {"ok": False, "error": f"unknown stimulus; valid: {sorted(STIMULI)}"}
    emotion_engine.stimulate(event, scale)
    return {"ok": True, "state": emotion_engine.to_dict()}


@app.get("/api/autonomy/context")
async def get_autonomy_context():
    """
    Return the perception context string Spider would see right now.
    Useful for debugging — shows exactly what the LLM is told each autonomy tick.
    """
    from robot.perception import get_context, get_recent_actions
    ctx = await get_context()
    return {
        "context":        ctx,
        "recent_actions": get_recent_actions(),
        "llm_mode":       autonomy_engine.llm_available,
        "locked_out_for": max(0.0, round(_COMMAND_LOCKOUT - (time.time() - autonomy_engine._last_commanded_at), 1)),
    }


@app.post("/api/autonomy/enable")
async def set_autonomy_enabled(enabled: bool = True):
    autonomy_engine.set_enabled(enabled)
    return {"ok": True, "enabled": enabled}


@app.get("/api/location")
async def get_location():
    """Return Spider's current estimated location: room, dead-reckoning pose, RSSI cache."""
    from robot.pose import pose_tracker
    return pose_tracker.to_dict()


@app.post("/api/location/calibrate")
async def calibrate_location(room: str):
    """Save current RSSI scan as fingerprint for this room (call once per room to calibrate)."""
    from robot.pose import pose_tracker
    pose_tracker.save_fingerprint(room)
    return {"ok": True, "room": room, "ssids_captured": len(pose_tracker._rssi_cache)}


@app.post("/api/location/reset")
async def reset_location(room: str = "living_room"):
    """Reset dead-reckoning to origin (e.g. after returning robot to charger)."""
    from robot.pose import pose_tracker
    pose_tracker.reset_pose(room)
    return {"ok": True, "room": room}


@app.get("/api/ports")
async def list_ports():
    """List available serial ports — useful for settings UI."""
    import serial.tools.list_ports
    ports = [{"port": p.device, "desc": p.description}
             for p in serial.tools.list_ports.comports()]
    return {"ports": ports}


# ---------------------------------------------------------------------------
# REST command endpoints — also callable from keyboard shortcuts in UI
# ---------------------------------------------------------------------------
@app.post("/api/cmd/walk/{direction}")
async def cmd_walk(direction: str):
    d = direction.upper()
    if d not in ("F", "B", "L", "R"):
        return {"ok": False, "error": "direction must be F B L R"}
    if d in ("F", "B"):
        await robot_comm.trot(d)   # trot gait for forward/back — matches wander behavior
    else:
        await robot_comm.walk(d)   # crawl turn gait for L/R
    return {"ok": True}


@app.post("/api/cmd/stop")
async def cmd_stop():
    await robot_comm.stop()
    return {"ok": True}


@app.post("/api/cmd/behavior/{state}")
async def cmd_behavior(state: str):
    valid = {"idle", "alert", "happy", "curious", "sleepy", "sleep"}
    if state not in valid:
        return {"ok": False, "error": f"state must be one of {valid}"}
    await robot_comm.set_behavior(state)
    return {"ok": True}


@app.post("/api/cmd/body")
async def cmd_body(x: int = 0, y: int = 0, z: int = 0):
    await robot_comm.move_body(x, y, z)
    return {"ok": True}


@app.post("/api/cmd/trot/{direction}")
async def cmd_trot(direction: str):
    if direction.upper() not in ("F", "B"):
        return {"ok": False, "error": "direction must be F or B"}
    await robot_comm.trot(direction)
    return {"ok": True}


@app.post("/api/cmd/speed/{level}")
async def cmd_speed(level: int):
    if not (1 <= level <= 10):
        return {"ok": False, "error": "speed must be 1–10"}
    await robot_comm.set_speed(level)
    return {"ok": True}


@app.post("/api/cmd/twist")
async def cmd_twist(mx: int = 0, my: int = 0, mz: int = 0,
                    rx: int = 0, ry: int = 0, rz: int = 0):
    await robot_comm.twist_body(mx, my, mz, rx, ry, rz)
    return {"ok": True}


@app.post("/api/cmd/diag")
async def cmd_diag():
    await robot_comm.request_diag()
    return {"ok": True}


# ---------------------------------------------------------------------------
# LLM action API — high-level, named actions for tool_use integration
# ---------------------------------------------------------------------------
class LLMActionRequest(BaseModel):
    action: str
    params: dict = {}


@app.post("/api/llm/act")
async def llm_act(body: LLMActionRequest):
    """
    Execute a high-level named action.
    Called by LLM tool_use blocks or any external controller.
    Returns {"ok": true, "result": "<description of what happened>"}.
    """
    result = await robot_actions.dispatch(body.action, body.params)
    return {"ok": True, "result": result}


@app.get("/api/llm/tools")
async def llm_tools():
    """Return the Anthropic-format tool schema for all available actions."""
    return {"tools": robot_actions.TOOLS}


@app.get("/api/llm/prompt")
async def llm_system_prompt():
    """Return the system prompt to inject when creating an LLM session for Spider."""
    return {"system_prompt": robot_actions.SYSTEM_PROMPT}


@app.post("/api/cmd/sleep")
async def cmd_sleep():
    await robot_comm.sleep()
    return {"ok": True}


@app.post("/api/cmd/wake")
async def cmd_wake():
    await robot_comm.wake()
    return {"ok": True}


# ---------------------------------------------------------------------------
# Servo calibration endpoints — require firmware CALI/CSOF support (BrainComm.cpp)
# ---------------------------------------------------------------------------
@app.post("/api/calibrate/prep")
async def calibrate_prep():
    """Move robot to raised calibration pose with servo offsets disabled.
    Also pauses autonomy so the LLM can't kick the robot out mid-session."""
    autonomy_engine.set_enabled(False)
    await robot_comm.cali_prep()
    return {"ok": True, "message": "Robot entering calibration pose — autonomy paused"}


@app.post("/api/calibrate/save")
async def calibrate_save():
    """Record current joint angles as calibration offsets and persist to EEPROM."""
    await robot_comm.cali_save()
    return {"ok": True, "message": "Offsets saved to EEPROM"}


@app.post("/api/calibrate/check")
async def calibrate_check():
    """Re-move to calibration points with offsets applied to verify alignment."""
    await robot_comm.cali_check()
    return {"ok": True, "message": "Verification move sent"}


@app.post("/api/calibrate/reset")
async def calibrate_reset():
    """Clear all 12 servo offset values (factory reset)."""
    await robot_comm.cali_reset()
    return {"ok": True, "message": "All servo offsets cleared"}


@app.post("/api/calibrate/servo")
async def calibrate_servo(leg: int, joint: str, value: float):
    """
    Set a single servo offset live (advanced / per-joint trim).
    leg: 1-4, joint: A (hip) / B (knee) / C (ankle), value: degrees ±15
    """
    if leg not in (1, 2, 3, 4):
        return {"ok": False, "error": "leg must be 1-4"}
    if joint.upper() not in ("A", "B", "C"):
        return {"ok": False, "error": "joint must be A, B, or C"}
    if not (-20 <= value <= 20):
        return {"ok": False, "error": "value must be between -20 and 20 degrees"}
    await robot_comm.set_servo_offset(leg, joint, value)
    return {"ok": True, "leg": leg, "joint": joint, "value": value}


@app.post("/api/calibrate/nudge")
async def calibrate_nudge(leg: int, dx: int = 0, dy: int = 0, dz: int = 0):
    """
    Move one leg's foot tip by a small XYZ delta (mm). Used by the cartesian
    calibration UI. Only honored while the robot is in CALIBRATE state.
    """
    if leg not in (1, 2, 3, 4):
        return {"ok": False, "error": "leg must be 1-4"}
    for v, name in ((dx, "dx"), (dy, "dy"), (dz, "dz")):
        if not (-30 <= v <= 30):
            return {"ok": False, "error": f"{name} must be in ±30 mm"}
    await robot_comm.cali_nudge(leg, dx, dy, dz)
    return {"ok": True, "leg": leg, "dx": dx, "dy": dy, "dz": dz}


@app.post("/api/calibrate/exit")
async def calibrate_exit():
    """Leave calibration mode and return the robot to IDLE. Resumes autonomy."""
    await robot_comm.cali_exit()
    autonomy_engine.set_enabled(True)
    return {"ok": True, "message": "Calibration exited — autonomy resumed"}


# ---------------------------------------------------------------------------
# Printable stickers (QR codes, 1 page)
# ---------------------------------------------------------------------------
@app.get("/print/stickers", response_class=HTMLResponse)
async def print_stickers():
    """
    Serve a single-page printable sheet of QR room stickers.
    Calibration template is the original Freenove PDF — see /print/calibration.
    """
    import io, base64, json as _json

    # Load room names from apartment_map.json
    map_path = os.path.join(os.path.dirname(__file__), "robot", "apartment_map.json")
    rooms: dict = {}
    if os.path.exists(map_path):
        with open(map_path, "r") as f:
            rooms = _json.load(f).get("rooms", {})

    # Generate QR code images (base64 PNG)
    qr_html = ""
    try:
        import qrcode
        for room_name, cfg in rooms.items():
            desc = cfg.get("description", room_name)
            img = qrcode.make(room_name)
            buf = io.BytesIO()
            img.save(buf, format="PNG")
            b64 = base64.b64encode(buf.getvalue()).decode()
            qr_html += f"""
<div class="sticker">
  <img src="data:image/png;base64,{b64}" alt="QR {room_name}">
  <div class="label">{desc}</div>
  <div class="code">{room_name}</div>
</div>"""
    except ImportError:
        qr_html = "<p style='color:red'>Run <code>pip install qrcode[pil]</code> to enable QR stickers.</p>"

    html = f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Spider Robot — Room Stickers</title>
<style>
  @media print {{
    @page {{ size: A4; margin: 10mm; }}
    .no-print {{ display: none; }}
  }}
  body {{ font-family: sans-serif; background: white; color: black; margin: 0; padding: 8mm; }}
  h2 {{ font-size: 11pt; margin: 0 0 2mm; border-bottom: 1px solid #ccc; padding-bottom: 1mm; }}
  p.hint {{ font-size: 9pt; color: #666; margin: 1mm 0 4mm; }}
  .sticker-grid {{ display: flex; flex-wrap: wrap; gap: 4mm; }}
  .sticker {{
    width: 42mm; height: 50mm;
    border: 1px dashed #888;
    display: flex; flex-direction: column; align-items: center; justify-content: center;
    padding: 2mm; box-sizing: border-box;
    page-break-inside: avoid;
  }}
  .sticker img {{ width: 30mm; height: 30mm; }}
  .sticker .label {{ font-size: 9pt; font-weight: bold; text-align: center; margin-top: 1mm; }}
  .sticker .code {{ font-size: 7pt; color: #888; }}
  .btn {{
    display: inline-block; padding: 5px 12px; background: #0066cc; color: white;
    border-radius: 4px; text-decoration: none; font-size: 10pt; cursor: pointer; border: none;
  }}
  .cal-link {{ margin-top: 6mm; font-size: 9pt; color: #444; }}
  .cal-link a {{ color: #0066cc; }}
</style>
</head>
<body>

<div class="no-print" style="margin-bottom:5mm;display:flex;align-items:center;gap:10px">
  <button class="btn" onclick="window.print()">Print stickers</button>
  <a class="btn" href="/print/calibration" target="_blank">Open calibration template (PDF)</a>
</div>

<h2>Room Stickers</h2>
<p class="hint">Cut along dashed lines. Stick on walls at ~10 cm height so the camera can see them.</p>
<div class="sticker-grid">
{qr_html}
</div>

<p class="cal-link no-print">
  For servo calibration, print the original Freenove template:
  <a href="/print/calibration" target="_blank">/print/calibration</a>
</p>

</body>
</html>"""
    return html


@app.get("/print/calibration")
async def print_calibration():
    """Serve the original Freenove V2 calibration graph PDF for printing."""
    pdf_path = (r"C:\Users\Admin\Downloads\Freenove_Quadruped_Robot_Kit-master"
                r"\Freenove_Quadruped_Robot_Kit-master\CalibrationGraph_for_V2.pdf")
    if not os.path.exists(pdf_path):
        from fastapi.responses import PlainTextResponse
        return PlainTextResponse(
            f"Calibration PDF not found at:\n{pdf_path}\n\n"
            "Find CalibrationGraph_for_V2.pdf in the Freenove kit folder and update the path in main.py.",
            status_code=404,
        )
    return FileResponse(pdf_path, media_type="application/pdf",
                        filename="CalibrationGraph_for_V2.pdf")


# ---------------------------------------------------------------------------
# WebSocket — bidirectional UI ↔ brain channel
# ---------------------------------------------------------------------------
@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await manager.connect(ws)
    # Send immediate state on connect
    await ws.send_text(json.dumps({"type": "state", "data": robot_state.to_dict()}))
    try:
        while True:
            raw = await ws.receive_text()
            await _handle_ws_message(raw)
    except WebSocketDisconnect:
        manager.disconnect(ws)


async def _handle_ws_message(raw: str):
    """Handle commands arriving from the browser over WebSocket."""
    try:
        msg = json.loads(raw)
    except json.JSONDecodeError:
        return

    action = msg.get("action")

    if action == "walk":
        d = msg.get("dir", "F").upper()
        if d in ("F", "B"):
            await robot_comm.trot(d)   # trot for forward/back
        else:
            await robot_comm.walk(d)   # crawl turn for L/R
    elif action == "stop":
        await robot_comm.stop()
    elif action == "behavior":
        await robot_comm.set_behavior(msg.get("state", "idle"))
    elif action == "body":
        await robot_comm.move_body(
            int(msg.get("x", 0)),
            int(msg.get("y", 0)),
            int(msg.get("z", 0)),
        )
    elif action == "rotate":
        await robot_comm.rotate_body(
            int(msg.get("x", 0)),
            int(msg.get("y", 0)),
            int(msg.get("z", 0)),
        )
    elif action == "trot":
        await robot_comm.trot(msg.get("dir", "F"))
    elif action == "twist":
        await robot_comm.twist_body(
            int(msg.get("mx", 0)), int(msg.get("my", 0)), int(msg.get("mz", 0)),
            int(msg.get("rx", 0)), int(msg.get("ry", 0)), int(msg.get("rz", 0)),
        )
    elif action == "speed":
        await robot_comm.set_speed(int(msg.get("level", 5)))
    elif action == "diag":
        await robot_comm.request_diag()
    elif action == "sleep":
        await robot_comm.sleep()
    elif action == "wake":
        await robot_comm.wake()
    elif action == "ping":
        t0 = time.time()
        await robot_comm.ping()
        robot_state.rtt_ms = (time.time() - t0) * 1000


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    uvicorn.run("main:app", host=SERVER_HOST, port=SERVER_PORT, reload=False)

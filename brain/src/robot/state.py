"""
Shared robot state — single source of truth, updated by comm layer,
read by WebSocket broadcaster and UI.
"""
import time
from dataclasses import dataclass, field, asdict
from typing import Optional


@dataclass
class LegPoint:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0


@dataclass
class RobotState:
    # Connection
    connected: bool = False
    connection_type: str = "none"      # "serial" | "wifi" | "none"

    # Robot reported state
    voltage: float = 0.0              # battery voltage in V
    behavior: str = "unknown"         # idle, alert, happy, curious, sleepy, sleep, walk_f ...
    busy: bool = False

    # Latency tracking
    last_seen: float = field(default_factory=time.time)
    rtt_ms: float = 0.0               # round-trip ping time

    # Diagnostic (from DIAG command)
    free_sram: int = 0                 # bytes
    uptime_s: int = 0                  # seconds since last reset

    # Leg positions (from future sensor/IK feedback — zeros until populated)
    leg1: LegPoint = field(default_factory=LegPoint)
    leg2: LegPoint = field(default_factory=LegPoint)
    leg3: LegPoint = field(default_factory=LegPoint)
    leg4: LegPoint = field(default_factory=LegPoint)

    # Event log (last 50 entries)
    events: list = field(default_factory=list)

    def update_from_report(self, report: dict):
        """Parse a state report dict from the robot firmware."""
        t = report.get("t")
        if t == "s":
            raw_v = report.get("v", 0)
            self.voltage  = raw_v / 100.0
            self.behavior = report.get("s", "unknown")
            self.busy     = bool(report.get("b", 0))
            self.last_seen = time.time()
            self.connected = True
        elif t == "pong":
            self.last_seen = time.time()
            self.connected = True
        elif t == "diag":
            self.free_sram = report.get("sram", 0)
            self.uptime_s  = report.get("up", 0)
        elif t == "evt":
            evt = report.get("e", "")
            self.add_event(evt)
            if _event_callback:
                _event_callback(evt)
            if _event_callback2:
                _event_callback2(evt)

    def add_event(self, msg: str):
        entry = {"ts": round(time.time() * 1000), "msg": msg}
        self.events.append(entry)
        if len(self.events) > 50:
            self.events.pop(0)

    def to_dict(self) -> dict:
        return {
            "connected":       self.connected,
            "connection_type": self.connection_type,
            "voltage":         round(self.voltage, 2),
            "behavior":        self.behavior,
            "busy":            self.busy,
            "rtt_ms":          round(self.rtt_ms, 1),
            "last_seen_ms":    round((time.time() - self.last_seen) * 1000),
            "legs": {
                "1": asdict(self.leg1),
                "2": asdict(self.leg2),
                "3": asdict(self.leg3),
                "4": asdict(self.leg4),
            },
            "events":          self.events[-20:],  # last 20 for UI
            "free_sram":       self.free_sram,
            "uptime_s":        self.uptime_s,
        }


# ---------------------------------------------------------------------------
# Event callbacks — primary (emotion engine) and secondary (brain console)
# ---------------------------------------------------------------------------
_event_callback  = None
_event_callback2 = None

def set_event_callback(cb, secondary: bool = False):
    global _event_callback, _event_callback2
    if secondary:
        _event_callback2 = cb
    else:
        _event_callback = cb


# Singleton — imported everywhere
robot_state = RobotState()

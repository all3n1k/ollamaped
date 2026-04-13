"""
Spider Emotion Engine
=====================
Models Spider's internal emotional state using the two-axis circumplex model:

  Valence  : -1.0 (distressed) ↔ +1.0 (pleased/content)
  Arousal  : -1.0 (dormant/sleepy) ↔ +1.0 (excited/agitated)

Both axes decay toward a time-of-day baseline over time.
Stimuli push the state; emotions have inertia and blend rather than snap.

Emotion quadrant → firmware behavior:

              AROUSAL +1
                   │
      ALERT/FEAR   │   EXCITED / HAPPY
   VALENCE -1 ─────┼───── VALENCE +1
      SLEEPY/SAD   │   CURIOUS / IDLE
                   │
              AROUSAL -1
"""

import time
import logging
from dataclasses import dataclass, field

log = logging.getLogger("robot.emotion")


def _clamp(v: float, lo: float = -1.0, hi: float = 1.0) -> float:
    return max(lo, min(hi, v))


# ---------------------------------------------------------------------------
# Stimuli — (dValence, dArousal) applied when each event fires
# ---------------------------------------------------------------------------
STIMULI: dict[str, tuple[float, float]] = {
    # social / positive
    "person_seen":       ( 0.40,  0.30),
    "face_recognized":   ( 0.55,  0.40),   # future: known person
    "touched":           ( 0.50,  0.20),   # future: IMU tap
    "command_received":  ( 0.20,  0.15),
    "play_started":      ( 0.30,  0.45),
    "play_finished":     ( 0.10, -0.10),
    "celebration":       ( 0.65,  0.70),

    # negative / stress
    "picked_up":         (-0.10,  0.60),   # future: IMU
    "put_down":          ( 0.10, -0.20),
    "loud_noise":        (-0.25,  0.65),   # future: mic
    "startled":          (-0.30,  0.50),
    "alone_long_time":   (-0.10, -0.10),   # loneliness

    # battery / hardware
    "low_battery":       (-0.35, -0.20),
    "battery_ok":        ( 0.10,  0.05),

    # environment (future: vision)
    "object_seen":       ( 0.20,  0.25),
    "new_environment":   ( 0.15,  0.35),
}


# ---------------------------------------------------------------------------
# Time-of-day baseline profiles
# ---------------------------------------------------------------------------
_BASELINES: list[tuple[tuple[int, int], tuple[float, float]]] = [
    # (hour_start, hour_end), (baseline_valence, baseline_arousal)
    ((6,  10), ( 0.20,  0.15)),   # morning: bright, energetic
    ((10, 18), ( 0.15,  0.00)),   # daytime: content, neutral
    ((18, 22), ( 0.10, -0.10)),   # evening: calm, warm
    ((22, 24), ( 0.00, -0.40)),   # night: sleepy
    ((0,   6), (-0.05, -0.55)),   # late night: very sleepy
]

def _time_baseline() -> tuple[float, float]:
    hour = time.localtime().tm_hour
    for (start, end), (bv, ba) in _BASELINES:
        if start <= hour < end:
            return bv, ba
    return 0.0, -0.4


# ---------------------------------------------------------------------------
# Emotion label derivation from (valence, arousal) position
# ---------------------------------------------------------------------------
def _derive_label(v: float, a: float) -> str:
    if a < -0.65:                           return "sleep"
    if a < -0.30:
        if v < -0.10:                       return "sleepy"
        if v <  0.20:                       return "idle"
        return "curious"                    # calm + positive = gentle curiosity
    if a >  0.50 and v >  0.30:             return "excited"
    if a >  0.15 and v >  0.10:             return "happy"
    if a >  0.15 and v < -0.05:             return "alert"
    if v >  0.15:                           return "curious"
    return "idle"


# Emotion label → firmware BEHAV string
_FIRMWARE_BEHAVIOR: dict[str, str] = {
    "excited": "excited",
    "happy":   "happy",
    "alert":   "alert",
    "curious": "curious",
    "idle":    "idle",
    "sleepy":  "sleepy",
    "sleep":   "sleep",
}


# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------
@dataclass
class EmotionState:
    valence: float = 0.10
    arousal: float = -0.10
    label:   str   = "idle"
    last_stimulus: str   = ""
    last_stimulus_at: float = field(default_factory=time.time)


class EmotionEngine:
    """
    Maintains Spider's emotional state.
    Call update() regularly for decay.
    Call stimulate(event) when something happens.
    """

    # Decay rates toward baseline (fraction per second)
    VALENCE_DECAY = 0.006
    AROUSAL_DECAY = 0.010

    def __init__(self):
        self.state = EmotionState()
        self._last_update = time.time()

    # ------------------------------------------------------------------
    def update(self):
        """Decay emotion toward time-of-day baseline. Call ~1Hz."""
        now = time.time()
        dt  = now - self._last_update
        self._last_update = now

        s = self.state
        bv, ba = self._get_baseline()

        s.valence = _clamp(s.valence + (bv - s.valence) * self.VALENCE_DECAY * dt)
        s.arousal = _clamp(s.arousal + (ba - s.arousal) * self.AROUSAL_DECAY * dt)
        s.label   = _derive_label(s.valence, s.arousal)

    def _get_baseline(self) -> tuple[float, float]:
        bv, ba = _time_baseline()
        # Low battery suppresses baseline arousal and valence
        try:
            from .state import robot_state
            v = robot_state.voltage
            if 0.5 < v < 6.5:
                bv -= 0.20
                ba -= 0.20
        except Exception:
            pass
        return _clamp(bv), _clamp(ba)

    # ------------------------------------------------------------------
    def stimulate(self, event: str, scale: float = 1.0):
        """
        Apply a named stimulus to push the emotional state.
        event: one of the keys in STIMULI
        scale: multiplier (0.0–2.0). Use <1.0 for weaker versions.
        """
        if event not in STIMULI:
            log.debug("unknown stimulus: %s", event)
            return
        dv, da = STIMULI[event]
        s = self.state
        s.valence = _clamp(s.valence + dv * scale)
        s.arousal = _clamp(s.arousal + da * scale)
        s.label   = _derive_label(s.valence, s.arousal)
        s.last_stimulus    = event
        s.last_stimulus_at = time.time()
        log.debug("stimulus %-20s → v=%.2f a=%.2f [%s]",
                  event, s.valence, s.arousal, s.label)

    # ------------------------------------------------------------------
    def get_firmware_behavior(self) -> str:
        """Map current emotion label to a firmware BEHAV command string."""
        return _FIRMWARE_BEHAVIOR.get(self.state.label, "idle")

    def to_dict(self) -> dict:
        s = self.state
        return {
            "valence":       round(s.valence, 3),
            "arousal":       round(s.arousal, 3),
            "label":         s.label,
            "last_stimulus": s.last_stimulus,
        }

    def summary(self) -> str:
        """One-line description for LLM context."""
        s = self.state
        return (
            f"emotion={s.label} "
            f"valence={s.valence:+.2f} arousal={s.arousal:+.2f}"
        )


# Singleton — imported by autonomy, main, actions
emotion_engine = EmotionEngine()

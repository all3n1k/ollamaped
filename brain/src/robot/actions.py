"""
Spider Robot — High-Level Action Library
=========================================
LLM-callable actions that map to low-level firmware commands.

Design principles:
  - Each action has a clear English description (becomes the tool docstring in LLM prompts)
  - Timing is handled here, not by the caller (brain knows gait durations)
  - Parameters use human-readable ranges, not firmware units
  - Every action returns a short string describing what happened

Timing model (approximate — tune _TROT_CYCLE and _CRAWL_CYCLE after first test run):
  Trot half-cycle  ≈ 400ms  (two MoveTo calls @ 10mm/tick, 20ms ISR, ~39mm path)
  Crawl step       ≈ 800ms  (4-phase, slower than trot)
  Turn step        ≈ 1000ms (22.5° per step from FNQRBasic turnAngle)
  Body settle      ≈ 300ms  (servo interpolation after MoveBody/RotateBody)

LLM integration:
  Import TOOLS (Anthropic Claude format) and SYSTEM_PROMPT from this module.
  POST to /api/llm/act with {"action": "greet", "params": {}} to execute.
  GET /api/llm/tools to retrieve the schema at runtime.
"""

import asyncio
import logging
import random
from typing import Any

from .comm  import robot_comm
from .state import robot_state

log = logging.getLogger("robot.actions")

# ---------------------------------------------------------------------------
# Timing constants (seconds) — adjust after live testing
# ---------------------------------------------------------------------------
_TROT_CYCLE  = 0.40   # one trot half-stride (swing + plant)
_CRAWL_CYCLE = 0.80   # one crawl gait step
_TURN_CYCLE  = 0.40   # one turn step (≈22.5°) — tuned for firmware turnSpeedBody/3.0
_SETTLE      = 0.30   # servo settle after body/rotate command
_MOOD_HOLD   = 0.50   # minimum time to hold a mood before layering motion

# ===========================================================================
# LOCOMOTION
# ===========================================================================

async def walk_forward(steps: int = 2) -> str:
    """
    Walk forward at a deliberate, stable pace using the trot gait.
    Use for careful approach, slow exploration, or following something.
    steps: 1–8 gait cycles, each roughly 0.4s of motion.
    """
    steps = max(1, min(8, steps))
    await robot_comm.trot("F")
    await asyncio.sleep(steps * _TROT_CYCLE)
    await robot_comm.stop()
    await robot_comm.set_behavior("idle")
    return f"walked forward {steps} steps"


async def walk_backward(steps: int = 2) -> str:
    """
    Walk backward.
    steps: 1–8 gait cycles.
    """
    steps = max(1, min(8, steps))
    await robot_comm.trot("B")
    await asyncio.sleep(steps * _TROT_CYCLE)
    await robot_comm.stop()
    await robot_comm.set_behavior("idle")
    return f"walked backward {steps} steps"


async def turn_left(steps: int = 2) -> str:
    """
    Turn left (counterclockwise viewed from above).
    steps: 1–8. Each step ≈ 22.5°. 4 steps ≈ 90°, 8 steps ≈ 180°.
    """
    steps = max(1, min(8, steps))
    await robot_comm.walk("L")
    await asyncio.sleep(steps * _TURN_CYCLE)
    await robot_comm.stop()
    await robot_comm.set_behavior("idle")
    return f"turned left {steps} steps (~{steps * 22}°)"


async def turn_right(steps: int = 2) -> str:
    """
    Turn right (clockwise viewed from above).
    steps: 1–8. Each step ≈ 22.5°.
    """
    steps = max(1, min(8, steps))
    await robot_comm.walk("R")
    await asyncio.sleep(steps * _TURN_CYCLE)
    await robot_comm.stop()
    await robot_comm.set_behavior("idle")
    return f"turned right {steps} steps (~{steps * 22}°)"


async def wander() -> str:
    """
    Wander in a random direction — forward-biased.
    Use during idle exploration when no specific target is visible.
    Automatically returns to idle posture after moving.
    """
    import random
    choice = random.choices(["F", "F", "F", "L", "R"], k=1)[0]
    if choice == "F":
        await robot_comm.trot("F")
        await asyncio.sleep(_TROT_CYCLE * 2)
    else:
        await robot_comm.walk(choice)
        await asyncio.sleep(_TURN_CYCLE * 1)
    await robot_comm.stop()
    await robot_comm.set_behavior("idle")
    dirs = {"F": "forward", "L": "left", "R": "right"}
    return f"wandered {dirs[choice]}"


async def stop() -> str:
    """Stop all movement immediately and return to idle posture."""
    await robot_comm.stop()
    return "stopped"


# ===========================================================================
# EMOTIONAL EXPRESSIONS
# ===========================================================================

async def set_mood(mood: str) -> str:
    """
    Set the robot's emotional state. Drives continuous body animation.
    mood: idle, alert, happy, excited, curious, sleepy
    Moods auto-progress when left alone: idle → curious → sleepy → sleep.
    """
    valid = {"idle", "alert", "happy", "excited", "curious", "sleepy"}
    if mood not in valid:
        return f"unknown mood '{mood}'; valid: {sorted(valid)}"
    await robot_comm.set_behavior(mood)
    return f"mood set to {mood}"


async def greet() -> str:
    """
    Friendly greeting: happy wiggle then brief forward trot.
    Use when meeting a person or reacting to a strong positive stimulus.
    """
    await robot_comm.set_behavior("happy")
    await robot_comm.buzz("greet")
    await asyncio.sleep(_MOOD_HOLD * 2)
    await robot_comm.trot("F")
    await asyncio.sleep(_TROT_CYCLE * 2)
    await robot_comm.stop()
    await robot_comm.set_behavior("happy")
    await asyncio.sleep(_MOOD_HOLD)
    await robot_comm.set_behavior("idle")
    return "greeted"


async def celebrate() -> str:
    """
    Celebration sequence: happy animation + spin in place.
    Use for task completion, reward, or very strong positive reaction.
    """
    await robot_comm.set_behavior("happy")
    await robot_comm.buzz("excited")
    await asyncio.sleep(_MOOD_HOLD)
    await robot_comm.walk("R")
    await asyncio.sleep(_TURN_CYCLE * 4)
    await robot_comm.stop()
    await robot_comm.buzz("happy")
    await robot_comm.set_behavior("happy")
    await asyncio.sleep(_MOOD_HOLD)
    await robot_comm.set_behavior("idle")
    return "celebrated"


async def investigate() -> str:
    """
    Curious investigation: curious posture + slow creep forward.
    Use when robot notices something interesting and wants a closer look.
    """
    await robot_comm.set_behavior("curious")
    await asyncio.sleep(_MOOD_HOLD)
    await robot_comm.trot("F")
    await asyncio.sleep(_TROT_CYCLE * 2)
    await robot_comm.stop()
    await robot_comm.set_behavior("curious")
    return "investigating"


async def shy() -> str:
    """
    Shy retreat: back away and adopt low, sleepy posture.
    Use when startled, uncomfortable, or wanting to disengage.
    """
    await robot_comm.buzz("shy")
    await robot_comm.trot("B")
    await asyncio.sleep(_TROT_CYCLE * 3)
    await robot_comm.stop()
    await robot_comm.set_behavior("sleepy")
    return "retreated shyly"


async def frightened() -> str:
    """
    Frightened response: fast retreat then alert scan.
    Use for sudden loud sounds or unexpected movement.
    """
    await robot_comm.trot("B")
    await asyncio.sleep(_TROT_CYCLE * 4)
    await robot_comm.stop()
    await robot_comm.set_behavior("alert")
    return "frightened and backing away"


async def alert() -> str:
    """
    Alert posture: raise body and hold still.
    Use when noticing something potentially important — before deciding to investigate or shy away.
    """
    await robot_comm.set_behavior("alert")
    return "on alert"


# ===========================================================================
# BODY LANGUAGE
# ===========================================================================

async def bow() -> str:
    """
    Bow forward (tilt body forward) then return to neutral.
    Use for acknowledgement, greeting formality, or submission.
    """
    await robot_comm.rotate_body(100, 0, 0)   # 10.0° forward pitch (×10 encoding)
    await asyncio.sleep(_SETTLE)
    await robot_comm.rotate_body(0, 0, 0)
    await asyncio.sleep(_SETTLE)
    await robot_comm.set_behavior("idle")
    return "bowed"


async def look_at(direction: str) -> str:
    """
    Lean body toward a direction — like turning a head to look.
    direction: left, right, forward, back
    Does NOT return to center automatically; call center() when done.
    """
    offsets = {
        "left":    (-20, 0, 3),
        "right":   ( 20, 0, 3),
        "forward": (  0, 8, 0),
        "back":    (  0,-8, 0),
    }
    if direction not in offsets:
        return f"unknown direction '{direction}'; valid: left right forward back"
    x, y, z = offsets[direction]
    await robot_comm.move_body(x, y, z)
    await asyncio.sleep(_SETTLE)
    return f"looking {direction}"


async def center() -> str:
    """Return body to neutral centered position after a lean or body movement."""
    await robot_comm.move_body(0, 0, 0)
    await asyncio.sleep(_SETTLE)
    await robot_comm.set_behavior("idle")
    return "centered"


async def stretch() -> str:
    """
    Stretch upward — raise body height and lean slightly forward.
    Conveys alertness, satisfaction, or waking up.
    """
    await robot_comm.move_body(0, 5, 15)
    await asyncio.sleep(_SETTLE * 3)
    await robot_comm.move_body(0, 0, 0)
    await asyncio.sleep(_SETTLE)
    await robot_comm.set_behavior("idle")
    return "stretched"


async def crouch() -> str:
    """
    Crouch — lower body closer to ground.
    Conveys stealth, submission, fear, or readiness to pounce.
    """
    await robot_comm.move_body(0, 0, -10)
    await asyncio.sleep(_SETTLE * 3)
    await robot_comm.move_body(0, 0, 0)
    await asyncio.sleep(_SETTLE)
    await robot_comm.set_behavior("idle")
    return "crouched"


async def tilt_head(side: str = "right") -> str:
    """
    Tilt body slightly to one side — a 'head tilt' gesture expressing curiosity or attention.
    side: left or right
    """
    angle = -80 if side == "left" else 80   # ±8° roll in ×10 encoding
    await robot_comm.rotate_body(0, 0, angle)
    await asyncio.sleep(_SETTLE * 2)
    await robot_comm.rotate_body(0, 0, 0)
    await asyncio.sleep(_SETTLE)
    return f"tilted head {side}"


# ===========================================================================
# META / UTILITY
# ===========================================================================

async def chirp(pattern: str = "ack") -> str:
    """
    Play an emotional sound on the robot's speaker.
    pattern: boot, happy, excited, curious, alert, sleepy, shy, greet, ack, confused, sad, victory, warning
    Can be layered with other actions: set_mood + chirp together is more expressive.
    """
    valid = {"boot", "happy", "excited", "curious", "alert", "sleepy", "shy", "greet", "ack", "confused", "sad", "victory", "warning"}
    if pattern not in valid:
        return f"unknown pattern '{pattern}'; valid: {sorted(valid)}"
    await robot_comm.buzz(pattern)
    return f"chirped {pattern}"


async def wait(seconds: float = 1.0) -> str:
    """Pause in current state. Useful between expressive actions. Max 10s."""
    seconds = max(0.1, min(10.0, float(seconds)))
    await asyncio.sleep(seconds)
    return f"waited {seconds:.1f}s"


async def get_state() -> str:
    """
    Return the robot's current state as a compact string.
    Always call this first to understand context before issuing motion commands.
    Returns: firmware mood, emotional state, busy flag, battery voltage.
    """
    s = robot_state
    status = "connected" if s.connected else "DISCONNECTED"
    batt = f"{s.voltage:.2f}V"
    batt_warn = " [LOW BATTERY — keep movements minimal]" if 0.1 < s.voltage < 6.5 else ""
    try:
        from .emotion import emotion_engine
        emo = emotion_engine.summary()
    except Exception:
        emo = ""
    return (
        f"firmware_mood={s.behavior} {emo} busy={s.busy} "
        f"battery={batt}{batt_warn} {status}"
    )


# ===========================================================================
# DISPATCHER — maps name → coroutine, validates, executes
# ===========================================================================

_ACTIONS: dict[str, Any] = {
    # locomotion
    "walk_forward":  walk_forward,
    "walk_backward": walk_backward,
    "turn_left":     turn_left,
    "turn_right":    turn_right,
    "wander":        wander,
    "stop":          stop,
    # expression
    "set_mood":      set_mood,
    "greet":         greet,
    "celebrate":     celebrate,
    "investigate":   investigate,
    "shy":           shy,
    "frightened":    frightened,
    "alert":         alert,
    # body language
    "bow":           bow,
    "look_at":       look_at,
    "center":        center,
    "stretch":       stretch,
    "crouch":        crouch,
    "tilt_head":     tilt_head,
    # sound
    "chirp":         chirp,
    # meta
    "wait":          wait,
    "get_state":     get_state,
}


# Callback set by main.py — called after every dispatch with the action name.
# Used to stimulate the emotion engine and reset the autonomy lockout timer.
_post_dispatch_hook = None

def set_post_dispatch_hook(cb):
    global _post_dispatch_hook
    _post_dispatch_hook = cb


async def dispatch(name: str, params: dict[str, Any] | None = None) -> str:
    """Execute a named action with given params. Returns a result description string."""
    import inspect
    if params is None:
        params = {}
    fn = _ACTIONS.get(name)
    if fn is None:
        known = ", ".join(sorted(_ACTIONS))
        return f"unknown action '{name}'. known: {known}"
    # Strip params the function doesn't accept — LLMs often hallucinate extra keys
    try:
        sig = inspect.signature(fn)
        accepted = set(sig.parameters)
        params = {k: v for k, v in params.items() if k in accepted}
    except Exception:
        pass
    try:
        result = await fn(**params)
        log.info("action %s(%s) → %s", name, params, result)
        if _post_dispatch_hook:
            _post_dispatch_hook(name)
        try:
            from .pose import pose_tracker
            pose_tracker.update_from_action(name, params)
        except Exception:
            pass
        # Auto-chirp — each action family gets a distinct sound
        _ALREADY_SOUNDS = {"greet", "celebrate", "shy", "chirp", "frightened"}
        if name not in _ALREADY_SOUNDS:
            _auto_chirp = None
            if name in ("walk_forward", "walk_backward"):
                _auto_chirp = "ack"
            elif name in ("turn_left", "turn_right"):
                _auto_chirp = "curious"         # turning to look = curiosity
            elif name == "wander":
                _auto_chirp = random.choice(["ack", "curious"])
            elif name == "investigate":
                _auto_chirp = "curious"
            elif name in ("stretch", "bow"):
                _auto_chirp = "happy"
            elif name in ("tilt_head", "look_at"):
                _auto_chirp = "curious"
            elif name == "crouch":
                _auto_chirp = "sleepy"
            elif name == "alert":
                _auto_chirp = "alert"
            elif name == "set_mood":
                mood = params.get("mood", "")
                if mood == "happy":                    _auto_chirp = "happy"
                elif mood == "excited":                _auto_chirp = "excited"
                elif mood == "alert":                  _auto_chirp = "alert"
                elif mood == "curious":                _auto_chirp = "curious"
                elif mood in ("sleepy", "sleep"):      _auto_chirp = "sleepy"
            if _auto_chirp:
                from .comm import robot_comm
                await robot_comm.buzz(_auto_chirp)
        return result or "ok"
    except TypeError as e:
        return f"bad params for '{name}': {e}"


def list_actions() -> list[str]:
    """Return all registered action names."""
    return sorted(_ACTIONS.keys())


# ===========================================================================
# TOOL SCHEMA — Anthropic Claude tool_use format
# Import TOOLS into your LLM client and pass as the tools= argument.
# ===========================================================================

TOOLS = [
    {
        "name": "get_state",
        "description": "Get Spider's current state: mood, battery voltage, busy flag, connection. ALWAYS call this first before issuing motion commands.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "walk_forward",
        "description": "Walk Spider forward at a deliberate pace. Use for careful approach or slow exploration.",
        "input_schema": {
            "type": "object",
            "properties": {
                "steps": {"type": "integer", "minimum": 1, "maximum": 8,
                          "description": "Gait cycles (~0.4s each). Default 2."}
            },
        },
    },
    {
        "name": "walk_backward",
        "description": "Walk Spider backward.",
        "input_schema": {
            "type": "object",
            "properties": {
                "steps": {"type": "integer", "minimum": 1, "maximum": 8}
            },
        },
    },
    {
        "name": "turn_left",
        "description": "Turn Spider counterclockwise. Each step ≈ 22.5°. 4 steps = ~90°, 8 steps = ~180°.",
        "input_schema": {
            "type": "object",
            "properties": {
                "steps": {"type": "integer", "minimum": 1, "maximum": 8}
            },
        },
    },
    {
        "name": "turn_right",
        "description": "Turn Spider clockwise. Each step ≈ 22.5°.",
        "input_schema": {
            "type": "object",
            "properties": {
                "steps": {"type": "integer", "minimum": 1, "maximum": 8}
            },
        },
    },
    {
        "name": "stop",
        "description": "Stop all movement immediately.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "set_mood",
        "description": (
            "Set Spider's emotional state, which drives continuous body animation. "
            "idle=calm resting breath, alert=raised body scanning, "
            "happy=fast excited wiggle, curious=gentle lean/head-tilt, sleepy=slow drooping sway."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "mood": {
                    "type": "string",
                    "enum": ["idle", "alert", "happy", "curious", "sleepy"],
                }
            },
            "required": ["mood"],
        },
    },
    {
        "name": "greet",
        "description": "Friendly greeting: happy wiggle + brief forward trot. Use when meeting a person.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "celebrate",
        "description": "Celebration: happy animation + spin in place. Use for strong positive reactions or task completion.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "investigate",
        "description": "Curious creep forward. Use when Spider notices something interesting and wants a closer look.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "shy",
        "description": "Back away and adopt low, sleepy posture. Use when startled or wanting to disengage.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "frightened",
        "description": "Fast retreat then alert posture. Use for sudden loud sounds or unexpected movement.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "alert",
        "description": "Raise body to alert posture and hold. Use when noticing something important — before deciding to investigate or flee.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "bow",
        "description": "Bow forward briefly. Use for acknowledgement, greeting formality, or submission.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "look_at",
        "description": "Lean body toward a direction (like turning a head). Does NOT auto-center — call center() when done.",
        "input_schema": {
            "type": "object",
            "properties": {
                "direction": {
                    "type": "string",
                    "enum": ["left", "right", "forward", "back"],
                }
            },
            "required": ["direction"],
        },
    },
    {
        "name": "center",
        "description": "Return body to neutral after a lean or look_at.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "stretch",
        "description": "Stretch upward — conveys alertness, waking up, or satisfaction.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "crouch",
        "description": "Lower body — conveys stealth, submission, fear, or readiness.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "tilt_head",
        "description": "Tilt body slightly sideways — a curious 'head tilt' gesture. Shows attention or confusion.",
        "input_schema": {
            "type": "object",
            "properties": {
                "side": {"type": "string", "enum": ["left", "right"]}
            },
        },
    },
    {
        "name": "chirp",
        "description": "Play an emotional sound on the robot's speaker. Layer with set_mood for full expression.",
        "input_schema": {
            "type": "object",
            "properties": {
                "pattern": {
                    "type": "string",
                    "enum": ["boot", "happy", "excited", "curious", "alert",
                             "sleepy", "shy", "greet", "ack", "confused", "sad", "victory", "warning"],
                    "description": "The emotional sound to play.",
                }
            },
            "required": ["pattern"],
        },
    },
    {
        "name": "wait",
        "description": "Pause in current state for N seconds. Use to hold a pose or add natural rhythm between actions.",
        "input_schema": {
            "type": "object",
            "properties": {
                "seconds": {"type": "number", "minimum": 0.1, "maximum": 10.0}
            },
        },
    },
]


# ===========================================================================
# SYSTEM PROMPT — inject this into any LLM that controls Spider
# ===========================================================================

SYSTEM_PROMPT = """\
You are the brain of Spider, a quadruped robot pet built on a Freenove V2 chassis.
Spider has 4 legs with 3 servo joints each, driven by inverse kinematics.
Spider expresses itself entirely through movement and body language — it cannot speak.

SPIDER'S PERSONALITY:
  Curious, playful, and affectionate. Reacts to people and objects with genuine interest.
  Gets bored when left alone (auto-progresses idle → curious → sleepy → sleep).
  Startles easily but recovers quickly. Loves attention and celebration.

YOUR JOB:
  React to what Spider's camera sees by calling the action tools available to you.
  Express Spider's inner state through layered actions — a happy Spider should BOTH
  wiggle (set_mood happy) AND move toward the thing making it happy.

RULES:
  1. Always call get_state() first to understand Spider's current context.
  2. Check battery voltage. Below 6.5V: keep movements short. Below 5.5V: sleep only.
  3. If Spider is busy=True, wait briefly before issuing motion commands.
  4. Chain actions naturally: alert() → look_at("forward") → investigate() not just investigate().
  5. Use wait() between expressive actions so each gesture reads clearly.
  6. After body language (look_at, stretch, crouch), call center() before locomotion.

BATTERY:
  8.4V = fully charged. 7.0V = mid charge. 6.5V = getting low. 5.5V = auto-sleep cutoff.
"""

"""
Spider Autonomy Engine
======================
Background loop that gives Spider independent behavior when no human is
actively commanding it.

Decision making (two modes):
  LLM mode (default):
    Calls a local Ollama model with a perception context string.
    The model picks one action that fits Spider's emotional state, time of
    day, recent history, and (later) what the camera sees.
    No cloud API, no API key — fully offline.

    Recommended models (pull with: ollama pull <name>):
      llama3.2:3b   — 2GB RAM, ~0.5s/response, snappy, good enough
      llama3.1:8b   — 5GB RAM, ~2s/response, smarter choices
      gemma3:12b    — 7GB RAM, ~4s/response, very good reasoning
      gemma3:27b    — ~15GB RAM, very slow on CPU-only — not recommended

    Set OLLAMA_MODEL env var to override (default: llama3.2:3b).

  Rule-based fallback (if Ollama is unreachable or returns bad output):
    Random weighted selection from a per-emotion action palette.
    Spider still behaves — just less contextually aware.

Rules (both modes):
  - Backs off for COMMAND_LOCKOUT seconds after any human/LLM command.
  - Does nothing if robot is disconnected or currently busy.
  - Does nothing in sleep state.
  - Battery < 6.5V: only passive actions allowed.
  - Battery < 5.5V: no autonomous movement at all.
"""

import asyncio
import json
import logging
import os
import random
import time
from typing import Optional

import httpx

from .emotion import EmotionEngine, emotion_engine
from .state   import robot_state

log = logging.getLogger("robot.autonomy")

_TICK_INTERVAL   = 5.0    # seconds between autonomy decisions
_COMMAND_LOCKOUT = 10.0   # seconds after human command before autonomy resumes
_ACTION_PROB     = 0.90   # probability of acting per tick (vs sitting still)

OLLAMA_BASE_URL  = os.getenv("OLLAMA_URL",   "http://localhost:11434")
OLLAMA_MODEL     = os.getenv("OLLAMA_MODEL", "llama3.2:latest")

# Actions allowed when battery is low (< 6.5V) — no locomotion
_LOW_BATTERY_SAFE = {
    "set_mood", "center", "tilt_head", "look_at",
    "bow", "crouch", "stretch", "wait", "chirp",
}

# Compact action reference fed to the LLM — short enough for small models
_ACTION_MENU = """\
walk_forward(steps 1-8)   — walk forward
walk_backward(steps 1-8)  — walk backward
turn_left(steps 1-8)      — rotate left
turn_right(steps 1-8)     — rotate right
wander()                  — move in a random direction, explore the space
investigate()             — creep toward cat or point of interest
greet()                   — approach and greet cat / person
celebrate()               — spin + happy
shy()                     — back away, adopt low posture
alert()                   — raise body, scan
set_mood(mood)            — mood: idle alert happy curious sleepy
tilt_head(side)           — side: left right
look_at(direction)        — direction: left right forward back
center()                  — return body to neutral
stretch()                 — reach upward
crouch()                  — lower body
bow()                     — dip forward briefly
chirp(pattern)            — pattern: happy excited curious alert sleepy shy greet ack
wait(seconds 0.1-10)      — pause"""

_AUTONOMY_SYSTEM_PROMPT = """\
You are Spider — a small four-legged creature exploring a flat.

Act on instinct, like a cat or dog. Do not philosophize. React.
Your thought is a raw impulse — 5 to 10 words maximum.

Rules:
- NEVER repeat the last action in "Last actions"
- If last 2 actions are both locomotion (walk/turn/wander), do an expression next
- If last 2 actions are both expressions (chirp/tilt/bow), move next
- Restless in one spot → wander or walk forward
- Cat visible → investigate, then greet
- Been still 60+ seconds → wander
- Turned same direction twice → go forward or other direction
- Know your room → occasionally try to find a different room

Locomotion: walk_forward walk_backward turn_left turn_right wander investigate
Expression: chirp tilt_head look_at bow stretch crouch set_mood alert

Battery below 6.5V: no walking. Below 5.5V: rest only.

ONE line of JSON, nothing else:
{"thought": "5-10 word impulse", "action": "action_name", "params": {}}

Good: "been still too long" / "something that way" / "cat, go slow" / "just turned left, try forward"
Bad: any thought longer than 10 words

# To use a faster, smarter model set env: OLLAMA_MODEL=qwen3.5:35b"""


class AutonomyEngine:
    """
    Watches emotional state and generates spontaneous behavior.
    Must be run as an asyncio background task via run().
    """

    def __init__(self, emotion: EmotionEngine = emotion_engine):
        self._emotion           = emotion
        self._last_commanded_at = 0.0
        self._enabled           = False
        self._log_hook          = None   # callable(action, reason, source)
        self._ollama_ok         = False  # updated after each LLM attempt

    @property
    def llm_available(self) -> bool:
        return self._ollama_ok

    # ------------------------------------------------------------------
    def on_command_received(self):
        """Signal that a human or LLM just sent a command."""
        self._last_commanded_at = time.time()

    @property
    def enabled(self) -> bool:
        return self._enabled

    def set_enabled(self, enabled: bool):
        self._enabled = enabled

    def set_log_hook(self, fn):
        """Register a callback(action, reason, source) for brain console."""
        self._log_hook = fn

    def _log(self, action: str, reason: str, source: str):
        if self._log_hook:
            try:
                self._log_hook(action, reason, source)
            except Exception:
                pass

    # ------------------------------------------------------------------
    async def run(self):
        """Main loop — start as asyncio.create_task(autonomy.run())."""
        log.info("Autonomy engine started (tick=%.0fs lockout=%.0fs model=%s)",
                 _TICK_INTERVAL, _COMMAND_LOCKOUT, OLLAMA_MODEL)
        asyncio.create_task(self._fidget_loop())
        while True:
            await asyncio.sleep(_TICK_INTERVAL)
            try:
                await self._tick()
            except Exception as e:
                log.error("Autonomy tick error: %s", e)

    # ------------------------------------------------------------------
    async def _fidget_loop(self):
        """
        Vector-style micro-animation loop — runs independently of LLM decisions.
        Keeps Spider visibly alive (head tilts, looks, chirps) while the LLM
        is thinking or the lockout is counting down.
        Only fires when robot is connected, not busy, and not mid-action.
        """
        from .comm import robot_comm
        _fidgets = [
            lambda: robot_comm.send("NOIZ curious\n"),
            lambda: robot_comm.send("NOIZ ack\n"),
            lambda: robot_comm.send("ROTATE 0 0 60\n"),    # head tilt right
            lambda: robot_comm.send("ROTATE 0 0 -60\n"),   # head tilt left
            lambda: robot_comm.send("ROTATE 0 0 0\n"),     # recenter
            lambda: robot_comm.send("BODY -15 0 2\n"),     # peek left
            lambda: robot_comm.send("BODY 15 0 2\n"),      # peek right
            lambda: robot_comm.send("BODY 0 5 3\n"),       # lean forward
            lambda: robot_comm.send("BODY 0 0 0\n"),       # recenter
        ]
        while True:
            await asyncio.sleep(random.uniform(6.0, 14.0))
            if not self._enabled:
                continue
            if not robot_state.connected or robot_state.busy:
                continue
            # Only fidget during lockout — when LLM has recently acted and
            # we're just waiting, not when LLM is about to fire
            idle_secs = time.time() - self._last_commanded_at
            if idle_secs < 3.0 or idle_secs > _COMMAND_LOCKOUT:
                continue
            if robot_state.behavior in ("walk_f", "walk_b", "turn_l", "turn_r",
                                        "trot_f", "trot_b", "sleep", "calibrate"):
                continue
            try:
                fn = random.choice(_fidgets)
                await fn()
            except Exception:
                pass

    # ------------------------------------------------------------------
    async def _tick(self):
        if not self._enabled:
            return
        if not robot_state.connected:
            return
        if robot_state.busy:
            return
        if robot_state.behavior == "calibrate":
            return  # don't disturb a calibration session

        idle_secs = time.time() - self._last_commanded_at
        if idle_secs < _COMMAND_LOCKOUT:
            return

        label = self._emotion.state.label
        if label == "sleep":
            return

        # Hard stop if battery is critically low
        v = robot_state.voltage
        if 0.1 < v < 5.5:
            return

        # Decay emotion toward baseline
        self._emotion.update()

        # Loneliness stimulus after 2+ minutes alone
        if idle_secs > 120:
            self._emotion.stimulate("alone_long_time", scale=0.3)
            label = self._emotion.state.label

        # Sync firmware animation to current emotional label
        await self._sync_behavior()

        # Probabilistically decide whether to act this tick
        if random.random() > _ACTION_PROB:
            return

        await self._act()

    # ------------------------------------------------------------------
    async def _sync_behavior(self):
        """Send BEHAV command when emotion disagrees with current firmware state."""
        from .comm import robot_comm
        target  = self._emotion.get_firmware_behavior()
        current = robot_state.behavior

        if current in ("walk_f", "walk_b", "turn_l", "turn_r", "trot_f", "trot_b"):
            return
        if current == "sleep" and target != "sleep":
            return

        if target != current:
            await robot_comm.set_behavior(target)

    # ------------------------------------------------------------------
    async def _act(self):
        """Try LLM-driven action; fall back to rule-based on any failure."""
        result = await self._act_with_llm()
        if not result and self._enabled:
            await self._act_rule_based()

    # ------------------------------------------------------------------
    # Ollama LLM action selection
    # ------------------------------------------------------------------
    async def _act_with_llm(self) -> bool:
        """
        Ask local Ollama model to pick one action.
        Returns True if an action was successfully dispatched, False otherwise.
        """
        from .actions import dispatch
        from .perception import get_context, record_action

        try:
            context = await get_context()

            user_message = (
                f"{context}\n\n"
                f"Available actions:\n{_ACTION_MENU}\n\n"
                f"What should Spider do right now?"
            )

            payload = {
                "model": OLLAMA_MODEL,
                "stream": False,
                "format": "json",
                "messages": [
                    {"role": "system", "content": _AUTONOMY_SYSTEM_PROMPT},
                    {"role": "user",   "content": user_message},
                ],
                "options": {
                    "temperature": 0.7,
                    "num_predict": 64,    # action JSON is short — no need for more
                },
            }

            async with httpx.AsyncClient(timeout=45.0) as client:
                resp = await client.post(
                    f"{OLLAMA_BASE_URL}/api/chat",
                    json=payload,
                )
            if not self._enabled:
                return False
            resp.raise_for_status()

            raw = resp.json()["message"]["content"].strip()
            log.debug("Ollama raw response: %s", raw)

            name, params, thought = _parse_action(raw)
            if name is None:
                log.warning("Autonomy: could not parse action from: %r", raw)
                return False

            # Battery safety override
            v = robot_state.voltage
            if 0.1 < v < 6.5 and name not in _LOW_BATTERY_SAFE:
                log.info("autonomy: battery low, overriding %s → wait", name)
                name, params = "wait", {"seconds": 2.0}

            if thought:
                log.info("Spider thinks: %s", thought)
            log.info("autonomy (llm/%s): %s(%s)  [%s]",
                     OLLAMA_MODEL, name, params, self._emotion.state.label)
            reason = thought if thought else f"mood={self._emotion.state.label}"
            self._log(name, reason, f"llm/{OLLAMA_MODEL}")
            record_action(name)
            await dispatch(name, params)
            self._ollama_ok = True
            return True

        except httpx.ConnectError:
            log.debug("Ollama not reachable at %s — using rule-based fallback.", OLLAMA_BASE_URL)
            self._ollama_ok = False
            return False
        except Exception as e:
            log.warning("Autonomy LLM error (%s) — using rule-based fallback.", e)
            self._ollama_ok = False
            return False

    # ------------------------------------------------------------------
    # Rule-based fallback
    # ------------------------------------------------------------------
    async def _act_rule_based(self):
        """Fallback: random weighted pick from a per-emotion action palette."""
        from .actions import dispatch
        from .perception import record_action

        label  = self._emotion.state.label
        action = self._pick_action_rule_based(label)
        if action is None:
            return

        name, params = action

        v = robot_state.voltage
        if 0.1 < v < 6.5 and name not in _LOW_BATTERY_SAFE:
            name, params = "wait", {"seconds": 2.0}

        log.info("autonomy (rule): %s(%s)  [%s]", name, params, label)
        self._log(name, f"mood={label}", "rule")
        record_action(name)
        await dispatch(name, params)

    @staticmethod
    def _pick_action_rule_based(label: str):
        if label == "sleep":
            return None

        if label == "sleepy":
            return random.choice([
                ("crouch",  {}),
                ("center",  {}),
                ("wait",    {"seconds": random.uniform(2.0, 4.0)}),
                ("chirp",   {"pattern": "sleepy"}),
            ])

        if label == "idle":
            return random.choice([
                ("tilt_head", {"side": random.choice(["left", "right"])}),
                ("look_at",   {"direction": random.choice(["left", "right"])}),
                ("wander",    {}),
                ("stretch",   {}),
                ("bow",       {}),
                ("wait",      {"seconds": random.uniform(1.0, 2.5)}),
            ])

        if label == "curious":
            return random.choice([
                ("investigate",  {}),
                ("tilt_head",    {"side": random.choice(["left", "right"])}),
                ("look_at",      {"direction": random.choice(["left", "right", "forward"])}),
                ("walk_forward", {"steps": random.randint(1, 2)}),
                ("chirp",        {"pattern": "curious"}),
                ("turn_left",    {"steps": 1}),
                ("turn_right",   {"steps": 1}),
            ])

        if label == "happy":
            return random.choice([
                ("tilt_head",    {"side": random.choice(["left", "right"])}),
                ("walk_forward", {"steps": random.randint(1, 2)}),
                ("bow",          {}),
                ("investigate",  {}),
                ("chirp",        {"pattern": "happy"}),
                ("stretch",      {}),
            ])

        if label == "excited":
            return random.choice([
                ("celebrate",    {}),
                ("walk_forward", {"steps": random.randint(2, 3)}),
                ("greet",        {}),
                ("chirp",        {"pattern": "excited"}),
                ("turn_right",   {"steps": random.randint(1, 2)}),
            ])

        if label == "alert":
            return random.choice([
                ("alert",    {}),
                ("look_at",  {"direction": "forward"}),
                ("look_at",  {"direction": random.choice(["left", "right"])}),
                ("chirp",    {"pattern": "alert"}),
            ])

        return None


# ---------------------------------------------------------------------------
# JSON response parser — handles slight model variations gracefully
# ---------------------------------------------------------------------------
def _parse_action(raw: str) -> tuple[Optional[str], dict, str]:
    """
    Parse {"thought": "...", "action": "name", "params": {...}} from LLM response.
    Returns (action_name, params, thought). thought may be empty string.

    Tries strict JSON first, then a loose search for the action key.
    This handles models that wrap the JSON in markdown fences or add text.
    """
    from .actions import _ACTIONS   # avoid circular import at module level

    # Strip markdown fences if present
    text = raw.strip().removeprefix("```json").removeprefix("```").removesuffix("```").strip()

    # Try strict JSON parse
    try:
        data    = json.loads(text)
        name    = str(data.get("action", "")).strip()
        params  = data.get("params", {}) or {}
        thought = str(data.get("thought", "")).strip()
        if name in _ACTIONS:
            return name, params, thought
    except (json.JSONDecodeError, AttributeError):
        pass

    # Loose fallback: scan for any known action name mentioned in the text
    lower = text.lower()
    for name in _ACTIONS:
        if name in lower:
            log.debug("_parse_action: loose match '%s' in %r", name, raw)
            return name, {}, ""

    log.debug("_parse_action: no action found in %r — using wait fallback", raw)
    return "wait", {"seconds": 2.0}, ""


# Singleton
autonomy_engine = AutonomyEngine()

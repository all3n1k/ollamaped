"""
Spider Perception Layer
=======================
Builds a natural-language context string for the autonomy LLM.

Vision runs on its own background loop (vision_loop) every VISION_INTERVAL
seconds — completely decoupled from the autonomy tick. get_context() reads
the cached moondream result instantly without ever waiting on the camera.
"""

import asyncio
import time
import logging
from collections import deque
from typing import Optional

log = logging.getLogger("robot.perception")

# ---------------------------------------------------------------------------
# Vision config
# ---------------------------------------------------------------------------
VISION_ENABLED   = True
VISION_INTERVAL  = 20.0   # seconds between moondream calls

# Shared state written by vision_loop(), read by get_context()
_vision_cache: Optional[str] = None
_vision_cache_at: float      = 0.0
_cat_visible: bool           = False

_CAT_KEYWORDS = {"cat", "kitten", "feline", "kitty"}

# ---------------------------------------------------------------------------
# Recent action memory
# ---------------------------------------------------------------------------
_recent_actions: deque[str] = deque(maxlen=6)

def record_action(name: str):
    _recent_actions.appendleft(name)

def get_recent_actions() -> list[str]:
    return list(_recent_actions)


# ---------------------------------------------------------------------------
# Background vision loop — runs independently, never blocks autonomy
# ---------------------------------------------------------------------------
async def vision_loop():
    """
    Capture a frame and run moondream every VISION_INTERVAL seconds.
    Stores result in module-level cache. Start as asyncio.create_task().
    """
    global _vision_cache, _vision_cache_at, _cat_visible

    log.info("Vision loop started (moondream every %.0fs)", VISION_INTERVAL)

    while True:
        if VISION_ENABLED:
            try:
                from vision import capture_frame, describe_frame
                from vision.markers import detect_qr
                jpg = await capture_frame()
                if jpg is None:
                    log.warning("Camera unreachable — moondream skipped (retrying in %.0fs)", VISION_INTERVAL)
                else:
                    # QR sticker check — instant location fix before moondream
                    qr = detect_qr(jpg)
                    if qr:
                        try:
                            from .pose import pose_tracker
                            pose_tracker.update_from_qr(qr)
                        except Exception:
                            pass
                        log.info("QR marker: %s", qr)

                    description = await describe_frame(jpg)
                    if description:
                        _vision_cache    = description
                        _vision_cache_at = time.time()
                        cat = any(k in description.lower() for k in _CAT_KEYWORDS)
                        if cat and not _cat_visible:
                            log.info("CAT DETECTED: %s", description)
                            # Immediate chirp on first detection
                            try:
                                from .comm import robot_comm
                                asyncio.create_task(robot_comm.buzz("curious"))
                            except Exception:
                                pass
                        elif not cat and _cat_visible:
                            log.info("Cat no longer visible")
                        _cat_visible = cat
                        log.info("Moondream: %s", description)
                        # Update room estimate from vision
                        try:
                            from .pose import pose_tracker
                            pose_tracker.update_from_vision(description)
                        except Exception:
                            pass
            except Exception as e:
                log.warning("Vision loop error: %s", e)

        await asyncio.sleep(VISION_INTERVAL)


# ---------------------------------------------------------------------------
# Main context builder — fully non-blocking, reads cached vision result
# ---------------------------------------------------------------------------
async def get_context() -> str:
    """
    Return a paragraph describing Spider's current situation.
    Injected into the autonomy LLM prompt. Never waits on camera or moondream.
    """
    parts: list[str] = []

    # --- Time of day --------------------------------------------------------
    hour = time.localtime().tm_hour
    if   6  <= hour < 10:  parts.append("It is morning.")
    elif 10 <= hour < 14:  parts.append("It is mid-morning.")
    elif 14 <= hour < 18:  parts.append("It is afternoon.")
    elif 18 <= hour < 22:  parts.append("It is evening.")
    elif 22 <= hour < 24:  parts.append("It is late at night.")
    else:                  parts.append("It is the middle of the night.")

    # --- Battery ------------------------------------------------------------
    try:
        from .state import robot_state
        v = robot_state.voltage
        if v > 7.5:
            parts.append(f"Battery good ({v:.1f}V).")
        elif v > 6.5:
            parts.append(f"Battery low ({v:.1f}V) — short movements only.")
        elif v > 0.1:
            parts.append(f"Battery critical ({v:.1f}V) — rest only.")
    except Exception:
        pass

    # --- Emotional state ----------------------------------------------------
    try:
        from .emotion import emotion_engine
        s = emotion_engine.state
        parts.append(f"Spider feels {s.label}.")
    except Exception:
        pass

    # --- Time alone ---------------------------------------------------------
    try:
        from .autonomy import autonomy_engine
        idle_secs = int(time.time() - autonomy_engine._last_commanded_at)
        if idle_secs > 600:
            parts.append(f"Alone for {idle_secs // 60} minutes.")
        elif idle_secs > 120:
            parts.append(f"Alone for {idle_secs}s.")
    except Exception:
        pass

    # --- Recent actions -----------------------------------------------------
    recent = get_recent_actions()
    if recent:
        parts.append(f"Last actions: {', '.join(recent)}.")

    # --- Vision (from background loop cache — never blocks) -----------------
    if _vision_cache:
        age = int(time.time() - _vision_cache_at)
        parts.append(f"Camera ({age}s ago): {_vision_cache}")
        if _cat_visible:
            parts.append("CAT VISIBLE — approach slowly.")

    # --- Location -----------------------------------------------------------
    try:
        from .pose import pose_tracker
        loc = pose_tracker.get_location_context()
        if loc:
            parts.append(loc)
    except Exception:
        pass

    return " ".join(parts)

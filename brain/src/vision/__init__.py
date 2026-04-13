"""
Spider Vision Pipeline
======================
Handles capturing and interpreting frames from the ESP32-CAM.

Current state: hardware not yet wired — all functions return None safely.
When ESP32-CAM arrives, set VISION_ENABLED = True and fill in the
implementation sections marked TODO.

How to enable vision (step by step):
  1. Wire ESP32-CAM to P3 connector:
       ESP32-CAM 5V    → P3 5V
       ESP32-CAM GND   → P3 GND
       ESP32-CAM U0TXD → P3 RX3 (pin 15 on ATmega)
       ESP32-CAM U0RXD → P3 TX3 (pin 14 on ATmega)

  2. Flash ESP32-CAM with standard CameraWebServer example from Arduino IDE.
     Board: "AI Thinker ESP32-CAM", partition: "Huge APP (3MB No OTA/1MB SPIFFS)".
     Set your WiFi SSID/password in the sketch. Upload via FTDI adapter.

  3. Find the camera's IP on your network (check router DHCP table).

  4. Set config below:
       VISION_ENABLED   = True
       CAMERA_IP        = "192.168.1.XXX"   # IP assigned to ESP32-CAM

  5. Choose a vision backend (see BACKEND options below) and set VISION_BACKEND.

  6. In perception.py, set VISION_ENABLED = True and CAMERA_SNAPSHOT_URL.

Architecture:
  autonomy._tick()
    → perception.get_context()
      → vision.get_scene_description()   ← this module
        → capture_frame()                 → GET /capture from ESP32-CAM
        → describe_frame(jpg_bytes)       → Ollama or Claude vision API
    → LLM picks action using context that now includes what camera sees
"""

import asyncio
import base64
import logging
import os
from typing import Optional

log = logging.getLogger("robot.vision")

# ---------------------------------------------------------------------------
# Config — edit these when hardware arrives
# ---------------------------------------------------------------------------
VISION_ENABLED = True
CAMERA_IP      = "192.168.1.210"
CAMERA_PORT    = 80
SNAPSHOT_PATH  = "/capture"  # ESP32-CAM snapshot endpoint

# Vision backend: "ollama" (local, private, recommended)
VISION_BACKEND = "ollama"
OLLAMA_URL     = os.getenv("OLLAMA_URL", "http://localhost:11434")
OLLAMA_MODEL   = "moondream"   # or "llava-phi3" — run: ollama pull moondream

# ---------------------------------------------------------------------------
# Public interface
# ---------------------------------------------------------------------------

async def get_scene_description() -> Optional[str]:
    """
    Capture a frame and return a one-sentence description of what Spider sees.
    Returns None if vision is disabled or the camera is unreachable.
    """
    if not VISION_ENABLED or not CAMERA_IP:
        return None

    jpg = await capture_frame()
    if jpg is None:
        return None

    return await describe_frame(jpg)


async def capture_frame() -> Optional[bytes]:
    """GET a JPEG snapshot from the ESP32-CAM. Retries once on failure."""
    if not CAMERA_IP:
        return None
    import httpx
    url = f"http://{CAMERA_IP}:{CAMERA_PORT}{SNAPSHOT_PATH}?framesize=7"  # SVGA 800×600
    for attempt in range(3):
        try:
            async with httpx.AsyncClient(timeout=12.0) as client:
                resp = await client.get(url)
            if resp.status_code == 200:
                return resp.content
            log.warning("Camera returned HTTP %d", resp.status_code)
            return None
        except Exception as e:
            if attempt < 2:
                log.debug("Camera capture attempt %d failed (%s), retrying...", attempt + 1, e)
                await asyncio.sleep(2.0)
            else:
                log.debug("Camera capture failed after 3 attempts: %s", e)
    return None


async def describe_frame(jpg: bytes) -> Optional[str]:
    """
    Send a JPEG frame to the configured vision backend and return
    a one-sentence description of the scene.
    """
    if VISION_BACKEND == "ollama":
        return await _describe_ollama(jpg)
    elif VISION_BACKEND == "claude":
        return await _describe_claude(jpg)
    else:
        log.warning("Unknown VISION_BACKEND: %s", VISION_BACKEND)
        return None


# ---------------------------------------------------------------------------
# Ollama backend (local, private — recommended)
# ---------------------------------------------------------------------------
async def _describe_ollama(jpg: bytes) -> Optional[str]:
    """
    Send frame to local Ollama moondream / llava-phi3.

    Prerequisites:
      - Ollama installed: https://ollama.com
      - Model pulled: `ollama pull moondream`
      - Ollama running: `ollama serve`

    i5-12500T with 16GB RAM can run moondream at ~1-2s per frame.
    """
    try:
        import httpx
        b64 = base64.b64encode(jpg).decode()
        payload = {
            "model":  OLLAMA_MODEL,
            "prompt": (
                "You are describing what a small robot's camera sees. "
                "Describe the scene in one short sentence. Focus on people, "
                "objects, and distance. Be brief."
            ),
            "images": [b64],
            "stream": False,
        }
        async with httpx.AsyncClient(timeout=10.0) as client:
            resp = await client.post(f"{OLLAMA_URL}/api/generate", json=payload)
        data = resp.json()
        description = data.get("response", "").strip()
        log.debug("Vision (ollama): %s", description)
        return description if description else None
    except Exception as e:
        log.debug("Ollama vision error: %s", e)
        return None


# ---------------------------------------------------------------------------
# Claude vision backend (cloud — needs ANTHROPIC_API_KEY)
# ---------------------------------------------------------------------------
async def _describe_claude(jpg: bytes) -> Optional[str]:
    """
    Send frame to Claude's vision API for scene description.
    Uses the same API key as the autonomy engine.
    Costs ~$0.001 per frame at Haiku pricing.
    """
    api_key = os.getenv("ANTHROPIC_API_KEY", "")
    if not api_key:
        log.warning("ANTHROPIC_API_KEY not set — cannot use Claude vision backend.")
        return None
    try:
        import anthropic
        b64 = base64.b64encode(jpg).decode()
        client = anthropic.AsyncAnthropic(api_key=api_key)
        response = await client.messages.create(
            model="claude-haiku-4-5-20251001",
            max_tokens=100,
            messages=[{
                "role": "user",
                "content": [
                    {
                        "type": "image",
                        "source": {
                            "type":       "base64",
                            "media_type": "image/jpeg",
                            "data":       b64,
                        },
                    },
                    {
                        "type": "text",
                        "text": (
                            "Describe what Spider's camera sees in one short sentence. "
                            "Focus on people, objects, and distance. Be brief."
                        ),
                    },
                ],
            }],
        )
        description = response.content[0].text.strip()
        log.debug("Vision (claude): %s", description)
        return description if description else None
    except Exception as e:
        log.debug("Claude vision error: %s", e)
        return None

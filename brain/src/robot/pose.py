"""
Spider Pose Tracker — three-layer localization without extra sensors.

Layer 1: Visual room detection
    moondream descriptions are matched against per-room keyword lists defined in
    apartment_map.json.  Updated every time the camera produces a description.

Layer 2: Dead reckoning
    Every dispatched action updates an x/y/heading estimate using the known
    firmware gait constants (trot stride = 30mm, turn step = 22.5°).
    Drifts over time but resets automatically when visual detection fires.

Layer 3: WiFi RSSI fingerprinting  (requires ESP32-CAM /rssi_scan endpoint)
    Brain polls ESP32-CAM every 30s for visible SSID/RSSI pairs.  Nearest-
    neighbour match against a calibration database stored in apartment_map.json.
    Activates only after the user runs the calibration workflow once per room.

Usage:
    from robot.pose import pose_tracker
    pose_tracker.load_map("path/to/apartment_map.json")
    pose_tracker.update_from_action("walk_forward", {"steps": 2})
    pose_tracker.update_from_vision("I see a couch and a television")
    print(pose_tracker.get_location_context())
"""

import json
import logging
import os
from math import cos, sin, radians
from typing import Optional

log = logging.getLogger("robot.pose")


class PoseTracker:
    # Firmware-derived gait constants (from FNQRBasic analysis)
    TROT_STRIDE_MM = 30.0   # full trot cycle (both diagonal pairs swing+land)
    TURN_DEGREES   = 22.5   # one WALK L/R step

    def __init__(self):
        # Dead-reckoning state
        self.x        = 0.0   # mm east from boot position
        self.y        = 0.0   # mm north from boot position
        self.heading  = 0.0   # degrees clockwise from initial facing (0 = forward)

        # Room estimate
        self.room            = "unknown"
        self.room_confidence = 0.0   # 0.0–1.0

        # Internal
        self._rssi_cache: dict[str, int] = {}   # {ssid: rssi_dbm} from last scan
        self._map: dict = {}
        self._map_path: Optional[str] = None

    # ------------------------------------------------------------------
    # Map loading
    # ------------------------------------------------------------------
    def load_map(self, path: str):
        self._map_path = path
        if not os.path.exists(path):
            log.warning("apartment_map.json not found at %s — localization disabled", path)
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                self._map = json.load(f)
            log.info("Apartment map loaded: %d rooms, %d RSSI fingerprints",
                     len(self._map.get("rooms", {})),
                     len(self._map.get("rssi_fingerprints", {})))
            # Start in home position if configured
            home = self._map.get("home_position")
            if home and home in self._map.get("rooms", {}):
                self.room = home
                self.room_confidence = 0.5
        except Exception as e:
            log.error("Failed to load apartment map: %s", e)

    def _save_map(self):
        if not self._map_path:
            return
        try:
            with open(self._map_path, "w", encoding="utf-8") as f:
                json.dump(self._map, f, indent=2)
        except Exception as e:
            log.error("Failed to save apartment map: %s", e)

    # ------------------------------------------------------------------
    # Layer 2: Dead reckoning — called from actions.dispatch()
    # ------------------------------------------------------------------
    def update_from_action(self, name: str, params: dict):
        """Update estimated pose after an action executes."""
        steps = int(params.get("steps", 2))
        if name == "walk_forward":
            dist = steps * self.TROT_STRIDE_MM
            self.x += dist * sin(radians(self.heading))
            self.y += dist * cos(radians(self.heading))
        elif name == "walk_backward":
            dist = steps * self.TROT_STRIDE_MM
            self.x -= dist * sin(radians(self.heading))
            self.y -= dist * cos(radians(self.heading))
        elif name == "turn_right":
            self.heading = (self.heading + steps * self.TURN_DEGREES) % 360
        elif name == "turn_left":
            self.heading = (self.heading - steps * self.TURN_DEGREES) % 360
        elif name == "wander":
            # Direction randomised inside wander() — approximate as 60mm forward
            dist = 60.0
            self.x += dist * sin(radians(self.heading))
            self.y += dist * cos(radians(self.heading))
        elif name == "investigate":
            # investigate() trots forward ~2 cycles
            dist = 2 * self.TROT_STRIDE_MM
            self.x += dist * sin(radians(self.heading))
            self.y += dist * cos(radians(self.heading))

    # ------------------------------------------------------------------
    # Layer 1: Visual room detection — called from perception.get_context()
    # ------------------------------------------------------------------
    def update_from_vision(self, description: str) -> Optional[str]:
        """
        Match a moondream description against room keywords.
        Returns the detected room name, or None if no match.
        """
        rooms = self._map.get("rooms", {})
        if not description or not rooms:
            return None
        lower = description.lower()
        scores: dict[str, int] = {}
        for room_name, cfg in rooms.items():
            hits = sum(1 for kw in cfg.get("visual_keywords", []) if kw in lower)
            if hits:
                scores[room_name] = hits
        if not scores:
            return None
        best      = max(scores, key=scores.get)
        new_conf  = min(scores[best] / 3.0, 1.0)   # 3 keywords = full confidence
        if best != self.room or new_conf > self.room_confidence:
            if best != self.room:
                log.info("Room changed: %s → %s (conf=%.2f)", self.room, best, new_conf)
            self.room            = best
            self.room_confidence = new_conf
        return self.room

    # ------------------------------------------------------------------
    # Layer 0: QR marker — highest priority, confidence=1.0
    # ------------------------------------------------------------------
    def update_from_qr(self, code: str):
        """
        Called when the camera detects a printed QR sticker.
        code = room name (e.g. "kitchen") or "room:landmark" (e.g. "kitchen:fridge").
        Sets confidence to 1.0 — deterministic fix.
        """
        room = code.split(":")[0].strip()
        if room in self._map.get("rooms", {}):
            old = self.room
            self.room = room
            self.room_confidence = 1.0
            if old != room:
                log.info("QR fix: %s → %s", old, room)
        else:
            log.debug("QR code %r not in room map — ignored", code)

    # ------------------------------------------------------------------
    # Layer 3: RSSI fingerprinting — called from rssi_poll_task in main.py
    # ------------------------------------------------------------------
    _MAX_RSSI_DIST = 25.0   # reject fingerprint matches weaker than this

    def update_from_rssi(self, scan: dict[str, int]):
        """
        Compare current RSSI scan to stored fingerprints using nearest-neighbour
        matching in RSSI space.  Only updates room when visual confidence is low.
        scan: {ssid: rssi_dbm}
        """
        self._rssi_cache = scan
        fingerprints = self._map.get("rssi_fingerprints", {})
        if not fingerprints or not scan:
            return

        best_room: Optional[str] = None
        best_dist = float("inf")
        for room_name, fp in fingerprints.items():
            shared = set(scan) & set(fp)
            if not shared:
                continue
            dist = sum((scan[s] - fp[s]) ** 2 for s in shared) ** 0.5
            if dist < best_dist:
                best_dist = dist
                best_room = room_name

        if best_room and best_dist < self._MAX_RSSI_DIST and self.room_confidence < 0.7:
            log.debug("RSSI match: %s (dist=%.1f)", best_room, best_dist)
            self.room            = best_room
            self.room_confidence = max(self.room_confidence, 0.35)

    def save_fingerprint(self, room: str):
        """Persist current RSSI scan as calibration fingerprint for a room."""
        if not self._rssi_cache:
            log.warning("No RSSI data cached — is ESP32-CAM /rssi_scan reachable?")
            return
        self._map.setdefault("rssi_fingerprints", {})[room] = dict(self._rssi_cache)
        self._save_map()
        log.info("Saved RSSI fingerprint for %s (%d SSIDs)", room, len(self._rssi_cache))

    # ------------------------------------------------------------------
    # Utilities
    # ------------------------------------------------------------------
    def reset_pose(self, room: Optional[str] = None):
        """Reset dead-reckoning origin. Call when robot returns to charger/home."""
        self.x = self.y = 0.0
        self.heading = 0.0
        if room:
            self.room            = room
            self.room_confidence = 1.0
        log.info("Pose reset — room: %s", self.room)

    def get_location_context(self) -> str:
        """Return a human-readable location sentence for the LLM context string."""
        parts = []
        if self.room != "unknown":
            conf_word = "confidently" if self.room_confidence > 0.6 else "probably"
            desc = (self._map.get("rooms", {})
                        .get(self.room, {})
                        .get("description", self.room))
            parts.append(f"Spider is {conf_word} in the {desc}.")

        dist_m = (self.x ** 2 + self.y ** 2) ** 0.5 / 1000.0
        if dist_m >= 0.3:
            parts.append(f"Spider has wandered {dist_m:.1f}m from its start position, "
                         f"facing {self._heading_label()}.")

        return " ".join(parts)

    def _heading_label(self) -> str:
        labels = ["forward", "right", "backward", "left"]
        idx = round(self.heading / 90) % 4
        return labels[idx]

    def to_dict(self) -> dict:
        return {
            "room":        self.room,
            "confidence":  round(self.room_confidence, 2),
            "x_mm":        round(self.x, 1),
            "y_mm":        round(self.y, 1),
            "heading_deg": round(self.heading, 1),
            "rssi_ssids":  len(self._rssi_cache),
        }


# Singleton
pose_tracker = PoseTracker()

"""
QR-code marker detection using OpenCV.

Stickers are printed from /print/stickers and placed on walls/furniture.
Each sticker encodes a room name (e.g. "kitchen") or "room:landmark"
(e.g. "living_room:couch").  The robot reads them during the vision loop
and gets a confidence=1.0 location fix.
"""
import logging
from typing import Optional

log = logging.getLogger("robot.vision.markers")

try:
    import cv2
    import numpy as np
    _detector = cv2.QRCodeDetector()
    _OPENCV_AVAILABLE = True
except ImportError:
    _OPENCV_AVAILABLE = False
    log.warning("opencv-python-headless not installed — QR detection disabled. "
                "Run: pip install opencv-python-headless")


def detect_qr(jpeg_bytes: bytes) -> Optional[str]:
    """
    Decode the first QR code found in a JPEG image.
    Returns the decoded string, or None if no QR code is found.
    """
    if not _OPENCV_AVAILABLE:
        return None
    try:
        arr = np.frombuffer(jpeg_bytes, np.uint8)
        img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
        if img is None:
            return None
        data, _, _ = _detector.detectAndDecode(img)
        if data:
            log.debug("QR decoded: %r", data)
            return data
        return None
    except Exception as e:
        log.debug("QR detection error: %s", e)
        return None

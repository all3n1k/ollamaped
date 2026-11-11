import asyncio
import importlib
from pathlib import Path
from unittest.mock import AsyncMock

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect


@pytest.fixture
def brain(monkeypatch):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1] / "brain/src"))
    return importlib.import_module("main")


def test_authentication_blocks_motion_and_websocket_upgrade(brain, monkeypatch):
    policy = brain.AccessPolicy({"DASHBOARD_HOST": "0.0.0.0", "DASHBOARD_TOKEN": "x" * 32})
    stop = AsyncMock()
    monkeypatch.setattr(brain, "access_policy", policy)
    monkeypatch.setattr(brain.robot_comm, "stop", stop)

    # No lifespan: the test client never opens a hardware transport.
    client = TestClient(brain.app, base_url="http://localhost")
    assert client.post("/api/cmd/stop").status_code == 401
    with pytest.raises(WebSocketDisconnect):
        with client.websocket_connect("/ws"):
            pytest.fail("Unauthorized dashboard connection accepted")
    stop.assert_not_awaited()


def test_manual_stop_cancels_pending_scan_and_disables_autonomy(brain, monkeypatch):
    async def exercise():
        started = asyncio.Event()

        async def pending_scan():
            started.set()
            await asyncio.sleep(60)

        task = asyncio.create_task(pending_scan())
        await started.wait()
        monkeypatch.setattr(brain, "boot_scan_task", task)
        stop = AsyncMock()
        monkeypatch.setattr(brain.robot_comm, "stop", stop)
        monkeypatch.setattr(brain.autonomy_engine, "_enabled", True)
        await brain.cmd_stop()
        assert task.cancelled()
        assert not brain.autonomy_engine.enabled
        stop.assert_awaited_once()

    asyncio.run(exercise())

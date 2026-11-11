"""
Async robot communication layer.
Reads JSON state reports from the robot, sends commands.
Supports USB serial and TCP (ESP8266 WiFi).
"""
import asyncio
import ipaddress
import json
import logging
import time
from typing import Optional, Callable

import serial
import serial.tools.list_ports

from .state import robot_state

log = logging.getLogger("robot.comm")

# Default config — overridden by main.py settings
SERIAL_PORT    = "COM3"
SERIAL_BAUD    = 115200
WIFI_HOST      = None   # e.g. "192.168.4.1"
WIFI_PORT      = 65535
RECONNECT_WAIT = 3.0    # seconds between reconnect attempts


class RobotComm:
    def __init__(self):
        self._ser: Optional[serial.Serial] = None
        self._tcp_reader: Optional[asyncio.StreamReader] = None
        self._tcp_writer: Optional[asyncio.StreamWriter] = None
        self._send_queue: asyncio.Queue = asyncio.Queue(maxsize=32)
        self._running = False
        self._on_state_update: Optional[Callable] = None

    def on_state_update(self, cb: Callable):
        """Register callback invoked whenever robot state changes."""
        self._on_state_update = cb

    # -------------------------------------------------------------------------
    # Public send interface — non-blocking, queued
    # -------------------------------------------------------------------------
    async def send(self, cmd: str):
        """Queue a command string for transmission."""
        try:
            self._send_queue.put_nowait(cmd.strip() + "\n")
        except asyncio.QueueFull:
            log.warning("Send queue full, dropping: %s", cmd)

    async def ping(self):
        await self.send("PING")

    async def walk(self, direction: str):
        """direction: F B L R"""
        await self.send(f"WALK {direction.upper()}")

    async def stop(self):
        # A saturated queue must never drop a stop or replay stale movement.
        while not self._send_queue.empty():
            self._send_queue.get_nowait()
        await self.send("STOP")

    async def set_behavior(self, state: str):
        await self.send(f"BEHAV {state.lower()}")

    async def move_body(self, x: int, y: int, z: int):
        await self.send(f"BODY {x} {y} {z}")

    async def rotate_body(self, x: int, y: int, z: int):
        await self.send(f"ROTATE {x} {y} {z}")

    async def trot(self, direction: str):
        """direction: F B"""
        await self.send(f"TROT {direction.upper()}")

    async def twist_body(self, mx: int, my: int, mz: int,
                         rx: int, ry: int, rz: int):
        """All values ×10 integers (e.g. 15 = 1.5 mm or 1.5°)."""
        await self.send(f"TWIST {mx} {my} {mz} {rx} {ry} {rz}")

    async def set_speed(self, speed: int):
        """speed: 1–10 (5 = normal)"""
        await self.send(f"SPEED {max(1, min(10, speed))}")

    async def buzz(self, pattern: str):
        """
        pattern: boot happy excited curious alert sleepy shy greet ack
        Plays an emotional chirp on the robot's onboard speaker.
        Firmware parses on first char 'N' → command must be NOIZ, not BUZZ.
        """
        await self.send(f"NOIZ {pattern.lower()}")

    async def request_diag(self):
        await self.send("DIAG")

    async def sleep(self):
        await self.send("SLEEP")

    async def wake(self):
        await self.send("WAKE")

    async def cali_prep(self):
        """Move robot to calibration pose with servo offsets disabled."""
        await self.send("CALI PREP")

    async def cali_save(self):
        """Record current leg positions as calibration offsets, save to EEPROM."""
        await self.send("CALI SAVE")

    async def cali_check(self):
        """Re-move to calibration points with offsets applied, verify alignment."""
        await self.send("CALI CHECK")

    async def cali_reset(self):
        """Clear all servo offset values (set all 12 joints to 0 offset)."""
        await self.send("CALI RESET")

    async def cali_nudge(self, leg: int, dx: int, dy: int, dz: int):
        """
        Move one leg's foot tip by the given delta in mm.
        leg: 1-4. Each delta clamped firmware-side to ±30mm.
        Only honored while the robot is in CALIBRATE state.
        """
        await self.send(f"CALI NUDGE {leg} {dx} {dy} {dz}")

    async def cali_exit(self):
        """Leave calibration mode and return to IDLE."""
        await self.send("CALI EXIT")

    async def set_servo_offset(self, leg: int, joint: str, value: float):
        """
        Set a single servo offset live (immediately visible, saved by cali_save).
        leg:   1-4
        joint: "A" (hip), "B" (knee), "C" (ankle)
        value: offset in degrees, range roughly ±15°
        """
        j = {"A": 0, "B": 1, "C": 2}.get(joint.upper())
        if j is None:
            return
        await self.send(f"CSOF {leg} {j} {int(value * 10)}")

    # -------------------------------------------------------------------------
    # Connection lifecycle
    # -------------------------------------------------------------------------
    async def run_serial(self, port: str = SERIAL_PORT, baud: int = SERIAL_BAUD):
        """Run serial connection loop — reconnects on disconnect."""
        self._running = True
        while self._running:
            try:
                log.info("Connecting to %s @ %d baud...", port, baud)
                loop = asyncio.get_event_loop()
                self._ser = await loop.run_in_executor(
                    None, lambda: serial.Serial(port, baud, timeout=0.1)
                )
                robot_state.connected = True
                robot_state.connection_type = "serial"
                robot_state.add_event(f"Connected via serial {port}")
                log.info("Serial connected")

                await asyncio.gather(
                    self._serial_reader(),
                    self._serial_writer(),
                    self._serial_watchdog(),
                )
            except serial.SerialException as e:
                log.warning("Serial error: %s", e)
            except Exception as e:
                log.error("Unexpected error: %s", e)
            finally:
                robot_state.connected = False
                robot_state.connection_type = "none"
                robot_state.add_event("Disconnected")
                if self._ser:
                    try: self._ser.close()
                    except: pass
                    self._ser = None
                if self._running:
                    log.info("Reconnecting in %.1fs...", RECONNECT_WAIT)
                    await asyncio.sleep(RECONNECT_WAIT)

    async def run_wifi(self, host: str, port: int = WIFI_PORT):
        """Run TCP/WiFi connection loop (brain connects outward to ESP)."""
        self._running = True
        while self._running:
            try:
                log.info("Connecting to %s:%d...", host, port)
                self._tcp_reader, self._tcp_writer = await asyncio.open_connection(host, port)
                robot_state.connected = True
                robot_state.connection_type = "wifi"
                robot_state.add_event(f"Connected via WiFi {host}:{port}")
                log.info("WiFi connected")

                await asyncio.gather(
                    self._tcp_reader_loop(self._tcp_reader),
                    self._tcp_writer_loop(self._tcp_writer),
                )
            except (ConnectionRefusedError, OSError) as e:
                log.warning("WiFi error: %s", e)
            finally:
                robot_state.connected = False
                robot_state.connection_type = "none"
                if self._tcp_writer:
                    try: self._tcp_writer.close()
                    except: pass
                if self._running:
                    await asyncio.sleep(RECONNECT_WAIT)

    async def run_tcp_server(self, port: int, bind: str, peer_ip: str):
        """
        TCP server mode — brain listens, ESP8266 connects inward.
        Accepts one robot connection at a time; waits for next on disconnect.
        """
        expected = ipaddress.ip_address(peer_ip)

        async def accept_robot(reader, writer):
            peer = writer.get_extra_info("peername")
            if not peer or ipaddress.ip_address(peer[0]) != expected or self._tcp_writer is not None:
                writer.close()
                await writer.wait_closed()
                return
            await self._handle_tcp_client(reader, writer)

        self._running = True
        server = await asyncio.start_server(accept_robot, bind, port)
        addr = server.sockets[0].getsockname()
        log.info("TCP server listening on %s:%d", addr[0], addr[1])
        robot_state.add_event(f"TCP server listening on port {port}")
        async with server:
            await server.serve_forever()

    async def _handle_tcp_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        """Called by asyncio for each incoming ESP8266 connection."""
        peer = writer.get_extra_info("peername")
        log.info("ESP8266 connected from %s:%d", peer[0], peer[1])
        # Drain any stale commands queued from the previous connection
        drained = 0
        while not self._send_queue.empty():
            try:
                self._send_queue.get_nowait()
                drained += 1
            except Exception:
                break
        if drained:
            log.debug("Drained %d stale commands from send queue on reconnect", drained)
        self._tcp_reader = reader
        self._tcp_writer = writer
        robot_state.connected = True
        robot_state.connection_type = "wifi"
        robot_state.add_event(f"ESP8266 connected from {peer[0]}")

        reader_task = asyncio.create_task(self._tcp_reader_loop(reader))
        writer_task = asyncio.create_task(self._tcp_writer_loop(writer))
        try:
            done, pending = await asyncio.wait(
                [reader_task, writer_task],
                return_when=asyncio.FIRST_COMPLETED,
            )
            for t in pending:
                t.cancel()
                try:
                    await t
                except (asyncio.CancelledError, Exception):
                    pass
        except Exception as e:
            log.warning("TCP client error: %s", e)
        finally:
            robot_state.connected = False
            robot_state.connection_type = "none"
            robot_state.add_event("ESP8266 disconnected")
            self._tcp_reader = None
            self._tcp_writer = None
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
            log.info("ESP8266 disconnected")

    def shutdown(self):
        self._running = False

    # -------------------------------------------------------------------------
    # Serial read/write coroutines
    # -------------------------------------------------------------------------
    async def _serial_reader(self):
        loop = asyncio.get_event_loop()
        buf = b""
        while self._running and self._ser and self._ser.is_open:
            try:
                chunk = await loop.run_in_executor(None, self._ser.read, 64)
                if not chunk:
                    await asyncio.sleep(0.005)
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self._parse_line(line.decode("ascii", errors="ignore").strip())
            except serial.SerialException:
                break

    async def _serial_watchdog(self):
        """Force-close serial if no data received in 8 seconds — breaks stuck gather."""
        while self._running and self._ser and self._ser.is_open:
            await asyncio.sleep(4.0)
            gap = time.time() - robot_state.last_seen
            if robot_state.connected and gap > 8.0:
                log.warning("Watchdog: no serial data for %.0fs, forcing reconnect", gap)
                try: self._ser.close()
                except: pass
                break

    async def _serial_writer(self):
        loop = asyncio.get_event_loop()
        while self._running and self._ser and self._ser.is_open:
            try:
                cmd = await asyncio.wait_for(self._send_queue.get(), timeout=1.0)
                await loop.run_in_executor(None, self._ser.write, cmd.encode())
            except asyncio.TimeoutError:
                pass
            except serial.SerialException:
                break

    # -------------------------------------------------------------------------
    # TCP read/write coroutines
    # -------------------------------------------------------------------------
    async def _tcp_reader_loop(self, reader: asyncio.StreamReader):
        while self._running:
            try:
                line = await reader.readline()
                if not line:  # EOF — remote closed connection
                    break
                self._parse_line(line.decode("ascii", errors="ignore").strip())
            except (asyncio.IncompleteReadError, ConnectionResetError, OSError):
                break

    async def _tcp_writer_loop(self, writer: asyncio.StreamWriter):
        while self._running:
            try:
                cmd = await asyncio.wait_for(self._send_queue.get(), timeout=1.0)
                writer.write(cmd.encode())
                await writer.drain()
            except asyncio.TimeoutError:
                pass
            except (ConnectionResetError, BrokenPipeError, OSError):
                break

    # -------------------------------------------------------------------------
    # Parse incoming line from robot
    # -------------------------------------------------------------------------
    def _parse_line(self, line: str):
        if not line:
            return
        try:
            data = json.loads(line)
            robot_state.update_from_report(data)
            if self._on_state_update:
                self._on_state_update(robot_state)
        except json.JSONDecodeError:
            pass  # Ignore non-JSON lines (boot messages, debug prints)


# Singleton
robot_comm = RobotComm()

@echo off
cd /d "%~dp0"
echo Spider Robot Brain Server
echo ==========================
echo Modes (set env vars before running):
echo   Serial (default):       set ROBOT_TCP_SERVER=0
echo   TCP server (WiFi):     set ROBOT_TCP_SERVER=1, ROBOT_TCP_BIND and ROBOT_TCP_PEER
echo   WiFi outbound:         set ROBOT_WIFI_HOST=192.168.x.x
echo.
echo Dashboard: http://localhost:8080
echo Robot TCP port: 9000 (if enabled)
echo.
if not defined ROBOT_TCP_SERVER set ROBOT_TCP_SERVER=0
.venv\Scripts\python.exe src\main.py
pause

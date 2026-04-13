@echo off
cd /d "%~dp0"
echo Spider Robot Brain Server
echo ==========================
echo Modes (set env vars before running):
echo   Serial (default):       set ROBOT_TCP_SERVER=0
echo   TCP server (WiFi):     just run this bat (default)
echo   WiFi outbound:         set ROBOT_WIFI_HOST=192.168.x.x
echo.
echo Dashboard: http://localhost:8080
echo Robot TCP port: 9000 (default mode)
echo.
set ROBOT_TCP_SERVER=1
.venv\Scripts\python.exe src\main.py
pause

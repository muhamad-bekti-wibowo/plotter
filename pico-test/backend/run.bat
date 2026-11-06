@echo off
rem Jalankan backend Kontrol Motor Pico (buka http://127.0.0.1:8765 di browser)
cd /d "%~dp0"
py -3.12 -m pip install -q -r requirements.txt
py -3.12 server.py
pause

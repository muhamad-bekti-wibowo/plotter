# Plotter CoreXY — ESP32 (ESP-IDF) + aplikasi kontrol Qt

- `esp-plotter/` — firmware ESP32 (ESP-IDF 5.5): gerak CoreXY, servo pena, WiFi/TCP/web, setelan di NVS, file job di flash (SPIFFS).
- `qt-plotter/` — aplikasi kontrol Qt 6 (C++): USB serial / WiFi, jog, kalibrasi, impor SVG & gambar (trace ke garis), mirror, stream ke plotter.
- `pico-test/` — referensi awal (Raspberry Pi Pico, Pico SDK).

## Build
- Firmware: `esp-plotter\build.ps1` (flash: `esp-plotter\build.ps1 "-p COMx flash"`).
- Aplikasi Qt: `qt-plotter\build.bat` (MSVC + Qt 6.11.1), hasil `qt-plotter\build\QtPlotter.exe`. Uji logika: `qt-plotter\test.bat`.

## Pin default ESP32
STEP A=27, DIR A=14, STEP B=26, DIR B=25, EN=13, Servo=4, Saklar X=32, Saklar Y=33, LED=2.
Semua bisa diubah lewat `PINS ...` / `pinXX n` lalu `APPLY` dan `SAVE`. Pin strapping (0, 2, 5, 12, 15) ditolak untuk motor/servo/saklar.

## Koneksi
UART 115200, TCP port 5000, web port 80. Tanpa WiFi, ESP32 membuka AP `Plotter-XXXX` (password `plotter123`) di 192.168.4.1. Ketik `HELP` untuk daftar perintah.

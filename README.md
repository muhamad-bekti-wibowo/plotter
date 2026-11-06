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

## Firmware siap pakai (1 file)
`release/esp-plotter-full.bin` — gabungan bootloader + partition table + aplikasi. Flash ke ESP32 (4 MB) di alamat **0x0**:

```
python -m esptool --chip esp32 -p COMx -b 460800 write_flash 0x0 release/esp-plotter-full.bin
```

Atau pakai Flash Download Tool Espressif / https://espressif.github.io/esp-launchpad/ (custom bin, alamat 0x0). Setelah itu buka serial 115200 dan ketik `HELP`.
Setelan (NVS) dan file job (SPIFFS) dibuat otomatis saat pertama boot.

## Aplikasi siap pakai (1 file exe)
`release/QtPlotter.exe` — Windows 10/11 64-bit, tinggal klik dua kali (tidak perlu instal Qt atau Visual C++).
Saat pertama dijalankan, exe mengekstrak dirinya sekali ke `%LOCALAPPDATA%\EmbeddedPlotter\`, lalu membuka aplikasi. Peluncuran berikutnya langsung terbuka.
Data kalibrasi manual disimpan di `%APPDATA%\EmbeddedPlotter\QtPlotter\calibration_data.jsonl`.
Untuk menghapus: hapus folder `%LOCALAPPDATA%\EmbeddedPlotter`.

## Update firmware lewat WiFi (OTA)

Setelah firmware ber-OTA di-flash lewat kabel sekali (tabel partisi berubah jadi 2 slot aplikasi), update berikutnya bisa lewat WiFi:
tab **Flash** di aplikasi → isi IP ESP32 → **Pilih .bin & Update** → pilih `release/esp-plotter-ota.bin`.
Atau lewat terminal: `curl --data-binary @release/esp-plotter-ota.bin http://IP-ESP32/ota`.
Firmware baru yang crash saat boot dikembalikan otomatis ke versi sebelumnya. Perintah `OTA` menampilkan slot yang dipakai.

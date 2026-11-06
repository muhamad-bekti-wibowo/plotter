// Tabel pengaturan (preferensi) disimpan di NVS flash. Satuan: mm, mm/s, mm/s2.
// X(nama, bawaan, min, maks, keterangan)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SETTINGS_TABLE(X) \
  X(spmm, 80, 1, 5000, "Langkah per mm (microstep/pitch belt)") \
  X(vmax, 60, 1, 500, "Kecepatan maks pindah (pena naik) mm/s") \
  X(vstart, 5, 0.5, 100, "Kecepatan awal/akhir mm/s") \
  X(accel, 500, 10, 20000, "Percepatan mm/s2") \
  X(drawv, 15, 0.5, 300, "Kecepatan menggambar (stream MV) mm/s") \
  X(pulseUs, 4, 1, 50, "Lebar pulsa STEP (us)") \
  X(dirSetupUs, 10, 0, 100, "Jeda pin DIR sebelum STEP (us)") \
  X(invA, 0, 0, 1, "Balik arah motor A") \
  X(invB, 0, 0, 1, "Balik arah motor B") \
  X(enActiveLow, 1, 0, 1, "ENABLE aktif LOW (A4988/DRV8825 = 1)") \
  X(enMode, 0, 0, 2, "Driver: 0 otomatis, 1 selalu nyala, 2 selalu mati") \
  X(enDelayMs, 5, 0, 500, "Jeda setelah driver dinyalakan (ms)") \
  X(idleS, 10, 0, 3600, "Mode 0: matikan driver setelah diam N detik (0=tidak)") \
  X(limEn, 1, 0, 1, "Pengawal saklar batas saat gerak biasa") \
  X(limActiveLow, 1, 0, 1, "Saklar tertekan = pin LOW") \
  X(limDebounceUs, 150, 0, 5000, "Debounce saklar (us)") \
  X(hFast, 30, 1, 300, "Kecepatan homing mm/s") \
  X(hOff, 10, 0, 100, "Jarak titik nol dari saklar (mm)") \
  X(hSearch, 400, 10, 5000, "Jarak cari saklar maks (mm)") \
  X(hCenter, 1, 0, 1, "Setelah HOME pindah ke tengah area") \
  X(hdx, -1, -1, 1, "Arah saklar X (-1 = X minimum)") \
  X(hdy, -1, -1, 1, "Arah saklar Y (-1 = Y minimum)") \
  X(areaW, 100, 1, 5000, "Lebar area kerja (mm)") \
  X(areaH, 100, 1, 5000, "Tinggi area kerja (mm)") \
  X(svUp, 0, 0, 180, "Servo: sudut pena naik") \
  X(svDn, 90, 0, 180, "Servo: sudut pena turun") \
  X(svMinUs, 500, 100, 3000, "Servo: pulsa sudut 0 (us)") \
  X(svMaxUs, 2500, 500, 3000, "Servo: pulsa sudut 180 (us)") \
  X(penMs, 300, 0, 3000, "Jeda setelah pena naik/turun (ms)") \
  X(pinSA, 27, 0, 39, "Pin STEP motor A (perlu APPLY)") \
  X(pinDA, 14, 0, 39, "Pin DIR motor A (perlu APPLY)") \
  X(pinSB, 26, 0, 39, "Pin STEP motor B (perlu APPLY)") \
  X(pinDB, 25, 0, 39, "Pin DIR motor B (perlu APPLY)") \
  X(pinEN, 13, 0, 39, "Pin ENABLE driver (perlu APPLY)") \
  X(pinSV, 4, 0, 33, "Pin servo (perlu APPLY)") \
  X(pinLX, 32, 0, 39, "Pin saklar X (perlu APPLY)") \
  X(pinLY, 33, 0, 39, "Pin saklar Y (perlu APPLY)")   X(pinLed, 2, -1, 33, "Pin LED indikator (-1 = mati, perlu APPLY)") \
  X(pinLX2, -1, -1, 39, "Pin saklar X kanan terpisah (-1 = kiri+kanan satu pin pinLX, perlu APPLY)") \
  X(pinLY2, -1, -1, 39, "Pin saklar Y atas terpisah (-1 = bawah+atas satu pin pinLY, perlu APPLY)")

enum {
#define X(n, d, a, b, s) S_##n,
  SETTINGS_TABLE(X)
#undef X
  S_COUNT
};

typedef struct { const char* name; float def, min, max; const char* desc; } setting_t;

extern float g_cfg[S_COUNT];
#define CFG(n) (g_cfg[S_##n])

void settings_init(void);                    // nvs_flash_init + muat dari NVS
const setting_t* settings_info(int i);
int settings_find(const char* name);         // tidak peka huruf besar/kecil, -1 jika tidak ada
void settings_set(int i, float v);           // dijepit ke [min,maks]
bool settings_is_pin(int i);
bool settings_save(void);                    // tulis ke NVS
void settings_reset_defaults(bool erase_nvs);

void wifi_cred_load(char* ssid, size_t sl, char* pass, size_t pl);
void wifi_cred_save(const char* ssid, const char* pass);
void ap_cred_load(char* ssid, size_t sl, char* pass, size_t pl);   // kosong = bawaan (Plotter-XXXX / plotter123)
void ap_cred_save(const char* ssid, const char* pass);

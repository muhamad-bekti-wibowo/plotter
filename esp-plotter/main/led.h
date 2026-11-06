// LED indikator status (bawaan GPIO2 = LED onboard devkit).
//  menyala terus       : plotter sedang bergerak/ada antrean
//  kedip cepat (5 Hz)  : WiFi belum tersambung (mode AP / mencoba)
//  kedip singkat 2 dtk : WiFi tersambung, diam
#pragma once
void led_start(void);
const char* led_apply(void);   // NULL jika sukses; baca pinLed dari setelan

// WiFi (STA dengan cadangan AP), server TCP port 5000.
#pragma once
#include <stdbool.h>
#include <stddef.h>
void net_start(void);                        // wifi + server TCP
void net_wifi_reconfigure(void);             // pakai kredensial terbaru dari NVS
void net_info(char* buf, size_t n);          // ringkasan status
bool net_sta_connected(void);

const char* net_set_wifi(const char* sta_ssid, const char* sta_pass, const char* ap_ssid, const char* ap_pass, bool commit);
void net_wifi_get(char* buf, size_t n);   // baris kunci=nilai (tanpa password)

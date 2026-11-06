// WiFi (STA dengan cadangan AP), server TCP port 5000.
#pragma once
#include <stdbool.h>
#include <stddef.h>
void net_start(void);                        // wifi + server TCP
void net_wifi_reconfigure(void);             // pakai kredensial terbaru dari NVS
void net_info(char* buf, size_t n);          // ringkasan status
bool net_sta_connected(void);

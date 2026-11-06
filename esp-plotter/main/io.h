// Transport & log: UART0, klien TCP, siaran pesan asinkron ("! ...").
#pragma once
#include <stddef.h>

void io_init(void);                          // UART + task tx
void io_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));   // siarkan "! teks" ke UART dan semua klien TCP
int io_client_add(int fd);                   // daftarkan socket klien untuk siaran (-1 jika penuh)
void io_client_remove(int fd);

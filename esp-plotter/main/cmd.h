// Parser perintah teks (protokol sama untuk UART, TCP, web, dan job di flash).
#pragma once
#include <stdio.h>
#include <stdbool.h>

typedef struct sink {
  void (*write)(struct sink* s, const char* line, size_t len);   // line sudah berakhir '\n'
  void* ctx;
  FILE* up;                                  // dipakai PUTF (unggah file lewat baris)
} sink_t;

enum { CMD_OK = 0, CMD_RETRY = 1, CMD_ERR = 2 };

void cmd_init(void);
int cmd_exec(const char* line, sink_t* s);   // thread-safe (mutex global)
bool job_start(const char* name);            // jalankan file di flash
void job_stop(void);
bool job_running(void);
void cmd_sink_close(sink_t* s);              // tutup file unggahan yang menggantung

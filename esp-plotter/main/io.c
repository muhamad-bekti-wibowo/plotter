#include "io.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "lwip/sockets.h"
#include "cmd.h"

#define TXL 160
#define MAX_CLIENTS 6
#define PC_UART UART_NUM_0

typedef struct { char s[TXL]; uint8_t n; } txmsg_t;

static QueueHandle_t txq;
static SemaphoreHandle_t uart_mx, cl_mx;
static int clients[MAX_CLIENTS];

static void uart_write_line(const char* l, size_t n) {
  xSemaphoreTake(uart_mx, portMAX_DELAY);
  uart_write_bytes(PC_UART, l, n);
  xSemaphoreGive(uart_mx);
}
static void uart_sink_write(sink_t* s, const char* l, size_t n) { (void)s; uart_write_line(l, n); }

void io_log(const char* fmt, ...) {
  if (!txq) return;
  txmsg_t m;
  m.s[0] = '!'; m.s[1] = ' ';
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(m.s + 2, TXL - 4, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > TXL - 5) n = TXL - 5;
  n += 2;
  m.s[n++] = '\n';
  m.n = (uint8_t)n;
  xQueueSend(txq, &m, 0);
}

int io_client_add(int fd) {
  int r = -1;
  xSemaphoreTake(cl_mx, portMAX_DELAY);
  for (int i = 0; i < MAX_CLIENTS; i++) if (!clients[i]) { clients[i] = fd; r = i; break; }
  xSemaphoreGive(cl_mx);
  return r;
}
void io_client_remove(int fd) {
  xSemaphoreTake(cl_mx, portMAX_DELAY);
  for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i] == fd) clients[i] = 0;
  xSemaphoreGive(cl_mx);
}

static void tx_task(void* arg) {
  txmsg_t m;
  for (;;) {
    if (xQueueReceive(txq, &m, portMAX_DELAY) != pdTRUE) continue;
    uart_write_line(m.s, m.n);
    xSemaphoreTake(cl_mx, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i]) send(clients[i], m.s, m.n, MSG_DONTWAIT);
    xSemaphoreGive(cl_mx);
  }
}

static void uart_rx_task(void* arg) {
  static sink_t us = {.write = uart_sink_write};
  char line[200];
  size_t len = 0;
  uint8_t b[64];
  for (;;) {
    int n = uart_read_bytes(PC_UART, b, sizeof b, pdMS_TO_TICKS(20));
    for (int i = 0; i < n; i++) {
      char ch = (char)b[i];
      if (ch == '\n' || ch == '\r') {
        if (len) { line[len] = 0; cmd_exec(line, &us); len = 0; }
        else if (us.up) { /* baris kosong saat unggah: abaikan */ }
      } else if (len < sizeof line - 1) line[len++] = ch;
    }
  }
}

void io_init(void) {
  uart_mx = xSemaphoreCreateMutex();
  cl_mx = xSemaphoreCreateMutex();
  txq = xQueueCreate(40, sizeof(txmsg_t));
  uart_config_t uc = {.baud_rate = 115200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
                      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
  uart_driver_install(PC_UART, 4096, 4096, 0, NULL, 0);
  uart_param_config(PC_UART, &uc);
  xTaskCreatePinnedToCore(tx_task, "io_tx", 3072, NULL, 5, NULL, 0);
  xTaskCreatePinnedToCore(uart_rx_task, "io_rx", 6144, NULL, 6, NULL, 0);
}

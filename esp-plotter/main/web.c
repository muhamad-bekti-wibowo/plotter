#include "web.h"
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "cmd.h"
#include "net.h"
#include "motion.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

typedef struct { char* buf; size_t len, cap; } mem_t;

static void mem_write(sink_t* s, const char* l, size_t n) {
  mem_t* m = s->ctx;
  if (m->len + n + 1 > m->cap) {
    if (m->cap > 16384) return;                                // batasi ukuran balasan
    size_t nc = (m->cap ? m->cap * 2 : 512) + n;
    char* nb = realloc(m->buf, nc);
    if (!nb) return;
    m->buf = nb; m->cap = nc;
  }
  memcpy(m->buf + m->len, l, n);
  m->len += n;
  m->buf[m->len] = 0;
}

static esp_err_t h_index(httpd_req_t* r) {
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  return httpd_resp_send(r, index_html_start, index_html_end - index_html_start);
}

// POST /cmd : badan berisi satu atau lebih baris perintah; balasan teks gabungan
static esp_err_t h_cmd(httpd_req_t* r) {
  if (r->content_len == 0 || r->content_len > 1024) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "badan 1..1024 byte");
  char* body = malloc(r->content_len + 1);
  if (!body) return httpd_resp_send_500(r);
  int got = 0;
  while (got < (int)r->content_len) {
    int n = httpd_req_recv(r, body + got, r->content_len - got);
    if (n <= 0) { free(body); return ESP_FAIL; }
    got += n;
  }
  body[got] = 0;
  mem_t m = {0};
  sink_t sk = {.write = mem_write, .ctx = &m};
  for (char *save, *ln = strtok_r(body, "\r\n", &save); ln; ln = strtok_r(NULL, "\r\n", &save)) cmd_exec(ln, &sk);
  cmd_sink_close(&sk);
  free(body);
  httpd_resp_set_type(r, "text/plain; charset=utf-8");
  esp_err_t e = httpd_resp_send(r, m.buf ? m.buf : "", m.len);
  free(m.buf);
  return e;
}

// POST /upload?name=file.job : simpan badan ke flash
static esp_err_t h_upload(httpd_req_t* r) {
  char q[96] = "", name[40] = "";
  httpd_req_get_url_query_str(r, q, sizeof q);
  if (httpd_query_key_value(q, "name", name, sizeof name) != ESP_OK || !name[0] || strchr(name, '/') || strstr(name, ".."))
    return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "name?");
  char path[64];
  snprintf(path, sizeof path, "/spiffs/%s", name);
  FILE* f = fopen(path, "w");
  if (!f) return httpd_resp_send_500(r);
  char b[512];
  int left = r->content_len;
  while (left > 0) {
    int n = httpd_req_recv(r, b, left < (int)sizeof b ? left : (int)sizeof b);
    if (n <= 0) { fclose(f); remove(path); return ESP_FAIL; }
    fwrite(b, 1, n, f);
    left -= n;
  }
  fclose(f);
  return httpd_resp_sendstr(r, "ok\n");
}

// POST /ota : badan = firmware .bin (esp-plotter.bin, bukan esp-plotter-full.bin). Tulis ke slot OTA lain, lalu restart.
static esp_err_t h_ota(httpd_req_t* r) {
  if (!motion_idle()) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "busy: plotter sedang bergerak");
  if (r->content_len < 65536) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "file terlalu kecil, bukan firmware");
  const esp_partition_t* p = esp_ota_get_next_update_partition(NULL);
  if (!p || r->content_len > p->size) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "file lebih besar dari slot OTA");
  motion_enable(false);
  esp_ota_handle_t oh;
  if (esp_ota_begin(p, r->content_len, &oh) != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin gagal");
  static char b[1024];
  int left = r->content_len;
  while (left > 0) {
    int n = httpd_req_recv(r, b, left < (int)sizeof b ? left : (int)sizeof b);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0 || esp_ota_write(oh, b, n) != ESP_OK) { esp_ota_abort(oh); return ESP_FAIL; }
    left -= n;
  }
  if (esp_ota_end(oh) != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "image tidak valid (pakai esp-plotter.bin)");
  if (esp_ota_set_boot_partition(p) != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot gagal");
  httpd_resp_sendstr(r, "ok restart\n");
  vTaskDelay(pdMS_TO_TICKS(800));
  esp_restart();
  return ESP_OK;
}

// GET /wifi : status (tanpa password). POST /wifi : badan 4 baris = SSID STA, password STA, SSID AP, password AP
// (password kosong = tidak diubah; SSID STA kosong = hapus STA, ESP32 jadi AP saja; SSID AP kosong = tidak diubah)
static esp_err_t h_wifi_get(httpd_req_t* r) {
  char b[200];
  net_wifi_get(b, sizeof b);
  httpd_resp_set_type(r, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(r, b);
}

static esp_err_t h_wifi_set(httpd_req_t* r) {
  if (r->content_len == 0 || r->content_len > 400) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "badan 1..400 byte");
  char body[420];
  int got = 0;
  while (got < (int)r->content_len) {
    int n = httpd_req_recv(r, body + got, r->content_len - got);
    if (n <= 0) return ESP_FAIL;
    got += n;
  }
  body[got] = 0;
  char* f[4] = {"", "", "", ""};
  char* p = body;
  for (int i = 0; i < 4 && p; i++) {
    f[i] = p;
    char* e = strchr(p, '\n');
    if (e) { *e = 0; p = e + 1; } else p = NULL;
    size_t l = strlen(f[i]);
    if (l && f[i][l - 1] == '\r') f[i][l - 1] = 0;
  }
  const char* err = net_set_wifi(f[0], f[1], f[2], f[3], false);
  if (err) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, err);
  httpd_resp_sendstr(r, "ok tersimpan, WiFi dihubungkan ulang");
  vTaskDelay(pdMS_TO_TICKS(400));                                   // beri waktu balasan terkirim sebelum WiFi berubah
  net_set_wifi(f[0], f[1], f[2], f[3], true);
  return ESP_OK;
}

void web_start(void) {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.stack_size = 8192;
  c.core_id = 0;
  httpd_handle_t h;
  if (httpd_start(&h, &c) != ESP_OK) return;
  httpd_uri_t u1 = {.uri = "/", .method = HTTP_GET, .handler = h_index};
  httpd_uri_t u2 = {.uri = "/cmd", .method = HTTP_POST, .handler = h_cmd};
  httpd_uri_t u3 = {.uri = "/upload", .method = HTTP_POST, .handler = h_upload};
  httpd_register_uri_handler(h, &u1);
  httpd_register_uri_handler(h, &u2);
  httpd_uri_t u4 = {.uri = "/ota", .method = HTTP_POST, .handler = h_ota};
  httpd_register_uri_handler(h, &u3);
  httpd_uri_t u5 = {.uri = "/wifi", .method = HTTP_GET, .handler = h_wifi_get};
  httpd_uri_t u6 = {.uri = "/wifi", .method = HTTP_POST, .handler = h_wifi_set};
  httpd_register_uri_handler(h, &u4);
  httpd_register_uri_handler(h, &u5);
  httpd_register_uri_handler(h, &u6);
}

#include "web.h"
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "cmd.h"

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
  httpd_register_uri_handler(h, &u3);
}

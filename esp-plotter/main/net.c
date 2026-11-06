#include "net.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "settings.h"
#include "cmd.h"
#include "io.h"

#define TCP_PORT 5000
#define EV_STA_UP (1 << 0)

static EventGroupHandle_t wev;
static TimerHandle_t retry_tmr;
static esp_netif_t *sta_if, *ap_if;
static char sta_ssid[40], sta_pass[70];
static char ip_str[16] = "-";
static int fail_count;
static bool ap_on;

static void start_ap(void) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  wifi_config_t ac = {0};
  snprintf((char*)ac.ap.ssid, sizeof ac.ap.ssid, "Plotter-%02X%02X", mac[4], mac[5]);
  ac.ap.ssid_len = (uint8_t)strlen((char*)ac.ap.ssid);
  strcpy((char*)ac.ap.password, "plotter123");
  ac.ap.max_connection = 4;
  ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ac.ap.channel = 1;
  esp_wifi_set_mode(sta_ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP);
  esp_wifi_set_config(WIFI_IF_AP, &ac);
  ap_on = true;
  io_log("wifi AP aktif: SSID=%s pass=plotter123 IP=192.168.4.1", ac.ap.ssid);
}

static void sta_connect(void) {
  wifi_config_t sc = {0};
  strlcpy((char*)sc.sta.ssid, sta_ssid, sizeof sc.sta.ssid);
  strlcpy((char*)sc.sta.password, sta_pass, sizeof sc.sta.password);
  esp_wifi_set_config(WIFI_IF_STA, &sc);
  esp_wifi_connect();
}

static void on_wifi(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (sta_ssid[0]) sta_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(wev, EV_STA_UP);
    strcpy(ip_str, "-");
    if (!sta_ssid[0]) return;
    if (++fail_count == 6 && !ap_on) start_ap();                     // cadangan: AP supaya tetap bisa dikonfigurasi
    xTimerStart(retry_tmr, 0);                                      // timer FreeRTOS: coba lagi tiap 10 detik
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
    snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&e->ip_info.ip));
    fail_count = 0;
    xEventGroupSetBits(wev, EV_STA_UP);
    io_log("wifi ip %s tcp=%d http=80", ip_str, TCP_PORT);
  }
}

static void retry_cb(TimerHandle_t t) {
  if (sta_ssid[0] && !(xEventGroupGetBits(wev) & EV_STA_UP)) esp_wifi_connect();
}

void net_wifi_reconfigure(void) {
  wifi_cred_load(sta_ssid, sizeof sta_ssid, sta_pass, sizeof sta_pass);
  fail_count = 0;
  esp_wifi_disconnect();
  if (sta_ssid[0]) {
    esp_wifi_set_mode(ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    sta_connect();
  } else start_ap();
}

bool net_sta_connected(void) { return wev && (xEventGroupGetBits(wev) & EV_STA_UP); }

void net_info(char* buf, size_t n) {
  wifi_mode_t m = WIFI_MODE_NULL;
  esp_wifi_get_mode(&m);
  snprintf(buf, n, "WIFI mode=%s ssid=%s ip=%s ap=%d tcp=%d", m == WIFI_MODE_STA ? "STA" : m == WIFI_MODE_AP ? "AP" : "APSTA",
           sta_ssid[0] ? sta_ssid : "-", ip_str, ap_on, TCP_PORT);
}

// ---------------------------------------------------------------- server TCP
typedef struct { int fd; } client_arg_t;

static void sock_sink_write(sink_t* s, const char* l, size_t n) {
  int fd = (int)(intptr_t)s->ctx;
  while (n) {
    int w = send(fd, l, n, 0);
    if (w <= 0) return;
    l += w; n -= (size_t)w;
  }
}

static void client_task(void* arg) {
  int fd = ((client_arg_t*)arg)->fd;
  free(arg);
  sink_t sk = {.write = sock_sink_write, .ctx = (void*)(intptr_t)fd, .up = NULL};
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  char line[200], b[128];
  size_t len = 0;
  for (;;) {
    int n = recv(fd, b, sizeof b, 0);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      if (b[i] == '\n' || b[i] == '\r') {
        if (len) { line[len] = 0; cmd_exec(line, &sk); len = 0; }
      } else if (len < sizeof line - 1) line[len++] = b[i];
    }
  }
  cmd_sink_close(&sk);
  io_client_remove(fd);
  close(fd);
  vTaskDelete(NULL);
}

static void server_task(void* arg) {
  int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(TCP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
  if (bind(ls, (struct sockaddr*)&a, sizeof a) || listen(ls, 4)) {
    io_log("error: gagal membuka TCP %d", TCP_PORT);
    vTaskDelete(NULL);
  }
  for (;;) {
    int fd = accept(ls, NULL, NULL);
    if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    if (io_client_add(fd) < 0) { const char m[] = "error: terlalu banyak klien\n"; send(fd, m, sizeof m - 1, 0); close(fd); continue; }
    client_arg_t* ca = malloc(sizeof *ca);
    ca->fd = fd;
    if (xTaskCreatePinnedToCore(client_task, "tcp_cl", 6144, ca, 6, NULL, 0) != pdPASS) { io_client_remove(fd); close(fd); free(ca); }
  }
}

void net_start(void) {
  wev = xEventGroupCreate();
  retry_tmr = xTimerCreate("wifi_retry", pdMS_TO_TICKS(10000), pdFALSE, NULL, retry_cb);
  esp_netif_init();
  esp_event_loop_create_default();
  sta_if = esp_netif_create_default_wifi_sta();
  ap_if = esp_netif_create_default_wifi_ap();
  (void)sta_if; (void)ap_if;
  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&ic);
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  wifi_cred_load(sta_ssid, sizeof sta_ssid, sta_pass, sizeof sta_pass);
  esp_wifi_set_mode(sta_ssid[0] ? WIFI_MODE_STA : WIFI_MODE_AP);
  if (!sta_ssid[0]) start_ap();
  esp_wifi_start();
  xTaskCreatePinnedToCore(server_task, "tcp_srv", 4096, NULL, 5, NULL, 0);
}

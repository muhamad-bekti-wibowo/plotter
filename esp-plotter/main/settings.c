#include "settings.h"
#include <string.h>
#include <strings.h>
#include "nvs_flash.h"
#include "nvs.h"

float g_cfg[S_COUNT];

static const setting_t k_tab[S_COUNT] = {
#define X(n, d, a, b, s) {#n, d, a, b, s},
  SETTINGS_TABLE(X)
#undef X
};

#define NS "plotter"

const setting_t* settings_info(int i) { return (i >= 0 && i < S_COUNT) ? &k_tab[i] : NULL; }

int settings_find(const char* name) {
  for (int i = 0; i < S_COUNT; i++)
    if (!strcasecmp(k_tab[i].name, name)) return i;
  return -1;
}

void settings_set(int i, float v) {
  if (i < 0 || i >= S_COUNT) return;
  if (v < k_tab[i].min) v = k_tab[i].min;
  if (v > k_tab[i].max) v = k_tab[i].max;
  g_cfg[i] = v;
}

bool settings_is_pin(int i) { return i >= S_pinSA && i <= S_pinLY2; }

void settings_reset_defaults(bool erase_nvs) {
  for (int i = 0; i < S_COUNT; i++) g_cfg[i] = k_tab[i].def;
  if (erase_nvs) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
      for (int i = 0; i < S_COUNT; i++) nvs_erase_key(h, k_tab[i].name);
      nvs_commit(h);
      nvs_close(h);
    }
  }
}

void settings_init(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  settings_reset_defaults(false);
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
  for (int i = 0; i < S_COUNT; i++) {          // kunci per nama: tahan terhadap perubahan tabel
    uint32_t u;
    if (nvs_get_u32(h, k_tab[i].name, &u) == ESP_OK) {
      float f;
      memcpy(&f, &u, 4);
      if (f == f) settings_set(i, f);          // tolak NaN
    }
  }
  nvs_close(h);
}

bool settings_save(void) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  for (int i = 0; i < S_COUNT; i++) {
    uint32_t u;
    memcpy(&u, &g_cfg[i], 4);
    nvs_set_u32(h, k_tab[i].name, u);
  }
  esp_err_t e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

void wifi_cred_load(char* ssid, size_t sl, char* pass, size_t pl) {
  ssid[0] = pass[0] = 0;
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
  size_t n = sl;
  if (nvs_get_str(h, "wssid", ssid, &n) != ESP_OK) ssid[0] = 0;
  n = pl;
  if (nvs_get_str(h, "wpass", pass, &n) != ESP_OK) pass[0] = 0;
  nvs_close(h);
}

void wifi_cred_save(const char* ssid, const char* pass) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, "wssid", ssid);
  nvs_set_str(h, "wpass", pass);
  nvs_commit(h);
  nvs_close(h);
}

void ap_cred_load(char* ssid, size_t sl, char* pass, size_t pl) {
  ssid[0] = pass[0] = 0;
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
  size_t n = sl;
  if (nvs_get_str(h, "apssid", ssid, &n) != ESP_OK) ssid[0] = 0;
  n = pl;
  if (nvs_get_str(h, "appass", pass, &n) != ESP_OK) pass[0] = 0;
  nvs_close(h);
}

void ap_cred_save(const char* ssid, const char* pass) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, "apssid", ssid);
  nvs_set_str(h, "appass", pass);
  nvs_commit(h);
  nvs_close(h);
}

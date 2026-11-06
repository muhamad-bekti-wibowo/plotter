// Plotter CoreXY di ESP32 (ESP-IDF). Core 1: pulsa STEP (task motion). Core 0: jaringan, parser, job.
#include "esp_spiffs.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "motion.h"
#include "cmd.h"
#include "io.h"
#include "net.h"
#include "web.h"
#include "led.h"

static void fs_init(void) {
  esp_vfs_spiffs_conf_t c = {.base_path = "/spiffs", .partition_label = "storage", .max_files = 6, .format_if_mount_failed = true};
  if (esp_vfs_spiffs_register(&c) != ESP_OK) io_log("error: SPIFFS gagal dipasang");
}

void app_main(void) {
  settings_init();      // NVS: preferensi
  cmd_init();
  io_init();
  fs_init();            // flash: data (job, config)
  motion_init();
  net_start();
  web_start();
  led_start();
  // OTA dengan rollback: firmware baru dianggap sehat bila sampai sini tanpa crash/reboot
  vTaskDelay(pdMS_TO_TICKS(3000));
  esp_ota_mark_app_valid_cancel_rollback();
  io_log("siap: esp-plotter 1.0 (ketik HELP)");
}

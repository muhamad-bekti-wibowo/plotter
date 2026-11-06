#include "led.h"
#include <math.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "motion.h"
#include "net.h"
#include "settings.h"

static int led_pin = -1;
static TimerHandle_t tmr;
static int tick;

static void led_cb(TimerHandle_t t) {
  if (led_pin < 0) return;
  tick++;
  int on;
  if (!motion_idle()) on = 1;
  else if (!net_sta_connected()) on = (tick / 1) % 2;              // 100 ms per tick -> 5 Hz
  else on = (tick % 20) == 0;                                      // kedip 100 ms tiap 2 detik
  gpio_set_level(led_pin, on);
}

const char* led_apply(void) {
  int p = (int)lroundf(CFG(pinLed));
  if (led_pin >= 0) { gpio_set_level(led_pin, 0); gpio_reset_pin(led_pin); }
  led_pin = -1;
  if (p < 0) return NULL;
  // pin 2 diizinkan khusus sebagai LED (output, tidak ditarik driver); pin lain ikut aturan motor
  bool bad = p == 0 || p == 1 || p == 3 || p == 5 || p == 12 || p == 15 || (p >= 6 && p <= 11) || p > 33 || (p >= 20 && p <= 24) || (p >= 28 && p <= 31);
  if (bad) return "pin LED tidak valid (boleh 2, 4, 13, 14, 16-19, 21-23, 25-27, 32, 33)";
  if (p == (int)lroundf(CFG(pinSA)) || p == (int)lroundf(CFG(pinDA)) || p == (int)lroundf(CFG(pinSB)) || p == (int)lroundf(CFG(pinDB)) ||
      p == (int)lroundf(CFG(pinEN)) || p == (int)lroundf(CFG(pinSV)) || p == (int)lroundf(CFG(pinLX)) || p == (int)lroundf(CFG(pinLY)) ||
      p == (int)lroundf(CFG(pinLX2)) || p == (int)lroundf(CFG(pinLY2)))
    return "pin LED bentrok dengan pin lain";
  gpio_reset_pin(p);
  gpio_set_direction(p, GPIO_MODE_OUTPUT);
  gpio_set_level(p, 0);
  led_pin = p;
  return NULL;
}

void led_start(void) {
  led_apply();
  tmr = xTimerCreate("led", pdMS_TO_TICKS(100), pdTRUE, NULL, led_cb);
  xTimerStart(tmr, 0);
}

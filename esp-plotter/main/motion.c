#include "motion.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "settings.h"
#include "io.h"

enum { MV_DONE, MV_HIT, MV_ABORT, MV_GUARD, MV_WRONG };

static QueueHandle_t q;
static SemaphoreHandle_t q_mx;
static EventGroupHandle_t ev;
#define EV_BUSY (1 << 0)

static volatile int32_t posA, posB;
static volatile uint32_t abort_seq;
static volatile int pending;
static volatile bool pen_dn, driver_on;
static int pSA = -1, pDA = -1, pSB = -1, pDB = -1, pEN = -1, pSV = -1, pLX = -1, pLY = -1, pLX2 = -1, pLY2 = -1;
static int32_t marks[4][2];       // posisi (A,B) penanda kalibrasi
static bool ledc_ready;

static inline int ipin(int idx) { return (int)lroundf(g_cfg[idx]); }

// ---------------------------------------------------------------- driver & servo
void motion_enable(bool on) {
  if (pEN >= 0) gpio_set_level(pEN, (on != (CFG(enActiveLow) != 0)) ? 1 : 0);   // aktif-LOW: on -> 0
  driver_on = on;
}

static void servo_us(int us) {
  if (!ledc_ready) return;
  uint32_t duty = (uint32_t)us * 16383u / 20000u;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void motion_servo_angle(float deg) {
  if (deg < 0) deg = 0;
  if (deg > 180) deg = 180;
  servo_us((int)(CFG(svMinUs) + (CFG(svMaxUs) - CFG(svMinUs)) * deg / 180.0f));
}

static void pen_set(bool dn) {
  motion_servo_angle(dn ? CFG(svDn) : CFG(svUp));
  pen_dn = dn;
  vTaskDelay(pdMS_TO_TICKS((int)CFG(penMs)));
}

const char* motion_apply_pins(void) {
  if (pending) return "busy";
  int n[10] = {ipin(S_pinSA), ipin(S_pinDA), ipin(S_pinSB), ipin(S_pinDB), ipin(S_pinEN), ipin(S_pinSV), ipin(S_pinLX), ipin(S_pinLY),
               ipin(S_pinLX2), ipin(S_pinLY2)};
  for (int i = 0; i < 10; i++) {
    int g = n[i];
    if (i >= 8 && g < 0) continue;                       // saklar kanan/atas boleh tidak ada
    if (g == 0 || g == 2 || g == 5 || g == 12 || g == 15) return "pin strapping (0,2,5,12,15) dilarang";
    if (g == 1 || g == 3 || (g >= 6 && g <= 11)) return "pin UART0/flash (1,3,6-11) dilarang";
    if (g < 0 || g > 39 || (g >= 20 && g <= 24) || (g >= 28 && g <= 31)) return "pin tidak ada di ESP32";
    if (i < 6 && g > 33) return "pin output harus <= 33 (34-39 hanya input)";
  }
  for (int i = 0; i < 10; i++)
    for (int j = i + 1; j < 10; j++)
      if (n[i] >= 0 && n[i] == n[j]) return "pin ganda";
  int old[10] = {pSA, pDA, pSB, pDB, pEN, pSV, pLX, pLY, pLX2, pLY2};
  for (int i = 0; i < 10; i++) if (old[i] >= 0) gpio_reset_pin(old[i]);
  pSA = n[0]; pDA = n[1]; pSB = n[2]; pDB = n[3]; pEN = n[4]; pSV = n[5]; pLX = n[6]; pLY = n[7]; pLX2 = n[8]; pLY2 = n[9];
  int outs[5] = {pSA, pDA, pSB, pDB, pEN};
  for (int i = 0; i < 5; i++) {
    gpio_reset_pin(outs[i]);
    gpio_set_direction(outs[i], GPIO_MODE_OUTPUT);
    gpio_set_level(outs[i], 0);
  }
  int ins[4] = {pLX, pLY, pLX2, pLY2};
  for (int i = 0; i < 4; i++) {
    if (ins[i] < 0) continue;
    gpio_reset_pin(ins[i]);
    gpio_set_direction(ins[i], GPIO_MODE_INPUT);
    if (ins[i] < 34) gpio_set_pull_mode(ins[i], CFG(limActiveLow) ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
  }
  ledc_timer_config_t tc = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_14_BIT, .timer_num = LEDC_TIMER_0,
                            .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK};
  ledc_timer_config(&tc);
  ledc_channel_config_t cc = {.gpio_num = pSV, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
  ledc_channel_config(&cc);
  ledc_ready = true;
  motion_servo_angle(pen_dn ? CFG(svDn) : CFG(svUp));
  motion_enable(CFG(enMode) == 1);
  return NULL;
}

// ---------------------------------------------------------------- saklar batas
static inline bool pin_pressed(int pin) { return gpio_get_level(pin) == (CFG(limActiveLow) != 0 ? 0 : 1); }
static inline uint32_t lim_state(uint32_t mask) {
  uint32_t s = 0;
  if ((mask & 1) && pLX >= 0 && pin_pressed(pLX)) s |= 1;
  if ((mask & 2) && pLY >= 0 && pin_pressed(pLY)) s |= 2;
  if ((mask & 4) && pLX2 >= 0 && pin_pressed(pLX2)) s |= 4;
  if ((mask & 8) && pLY2 >= 0 && pin_pressed(pLY2)) s |= 8;
  return s;
}
uint32_t motion_limits(void) { return lim_state(15); }
bool motion_has_max(void) { return pLX2 >= 0 && pLY2 >= 0; }
int motion_sw_pin(int sw) { return sw == 0 ? pLX : sw == 1 ? pLY : sw == 2 ? pLX2 : pLY2; }
int motion_raw_level(int sw) { int p = motion_sw_pin(sw); return p < 0 ? -1 : gpio_get_level(p); }

static inline bool lim_hit(uint32_t mask, int64_t* since, int64_t debounce) {
  if (!lim_state(mask)) { *since = 0; return false; }
  int64_t now = esp_timer_get_time();
  if (!*since) { *since = now; return false; }
  return (now - *since) >= debounce;
}

// seperti lim_hit, tapi saklar di *ignore diabaikan sampai terlihat lepas sekali (FL_REARM)
static inline bool lim_hit_ra(uint32_t mask, uint32_t* ignore, int64_t* since, int64_t debounce) {
  if (*ignore) *ignore &= lim_state(*ignore);
  return lim_hit(mask & ~*ignore, since, debounce);
}

// ---------------------------------------------------------------- satu gerakan lurus
static uint32_t guard_mask;

static int run_move(const mot_item_t* it, uint32_t seq0) {
  int32_t sa = it->a, sb = it->b, aa = abs(sa), bb = abs(sb), N = aa > bb ? aa : bb;
  if (!N) return MV_DONE;
  // gerak "cari saklar": awasi SEMUA saklar (yang sudah tertekan di awal diabaikan), supaya arah terbalik tidak menerobos saklar seberang
  uint32_t mask = it->stop ? (it->stop | (15 & ~lim_state(15))) : ((it->fl & FL_NOGUARD) || !CFG(limEn) ? 0 : (15 & ~lim_state(15)));
  int64_t since = 0, deb = (int64_t)CFG(limDebounceUs);
  uint32_t ignore = (it->stop && (it->fl & FL_REARM)) ? (it->stop & lim_state(it->stop)) : 0;
  bool masterA = aa >= bb;
  int32_t slv = masterA ? bb : aa;
  gpio_set_level(pDA, (sa > 0) != (CFG(invA) != 0));
  gpio_set_level(pDB, (sb > 0) != (CFG(invB) != 0));
  esp_rom_delay_us((uint32_t)CFG(dirSetupUs));

  float spmm = CFG(spmm);
  float dx = (sa + sb) * 0.5f, dy = (sa - sb) * 0.5f, dist = sqrtf(dx * dx + dy * dy);
  float k = dist > 0 ? (float)N / dist : 1.0f;                 // langkah motor master per langkah alat
  float vt = (it->v > 0 ? it->v : CFG(vmax)) * spmm * k;        // langkah master/detik
  float vs = fminf(CFG(vstart) * spmm * k, vt);
  float ac = CFG(accel) * spmm * k;
  float v0sq = vs * vs, vmsq = vt * vt;
  int32_t accSteps = (int32_t)((vmsq - v0sq) / (2.0f * ac));
  if (accSteps * 2 > N) accSteps = N / 2;
  uint32_t pulse = (uint32_t)CFG(pulseUs);
  int32_t err = N / 2;
  int64_t t = esp_timer_get_time();
  int pSa = pSA, pSb = pSB;

  for (int32_t i = 0; i < N; i++) {
    if (abort_seq != seq0) return MV_ABORT;
    if (mask && lim_hit_ra(mask, &ignore, &since, deb)) goto tripped;
    float v2;
    if (!(it->fl & FL_NO_ACCEL) && i < accSteps) v2 = v0sq + 2.0f * ac * i;
    else if (!(it->fl & FL_NO_DECEL) && i >= N - accSteps) v2 = v0sq + 2.0f * ac * (N - 1 - i);
    else v2 = vmsq;
    float v = sqrtf(v2);
    if (v < vs) v = vs;
    err += slv;
    bool st = false;
    if (err >= N) { err -= N; st = true; }
    bool sA = masterA ? true : st, sB = masterA ? st : true;
    if (sA) gpio_set_level(pSa, 1);
    if (sB) gpio_set_level(pSb, 1);
    esp_rom_delay_us(pulse);
    gpio_set_level(pSa, 0);
    gpio_set_level(pSb, 0);
    if (sA) posA += sa > 0 ? 1 : -1;
    if (sB) posB += sb > 0 ? 1 : -1;
    t += (int64_t)(1000000.0f / v);
    while (esp_timer_get_time() < t) {
      if (mask && lim_hit_ra(mask, &ignore, &since, deb)) goto tripped;
      if (abort_seq != seq0) return MV_ABORT;
    }
  }
  return MV_DONE;
tripped:
  if (it->stop) {
    uint32_t p = lim_state(mask & ~ignore);
    if (!p || (p & it->stop)) return MV_HIT;                 // saklar yang dicari tertekan
    guard_mask = p;
    return MV_WRONG;                                         // saklar LAIN tertekan: arah gerak terbalik
  }
  { uint32_t p = lim_state(mask); guard_mask = p ? p : mask; }
  return MV_GUARD;
}

// ---------------------------------------------------------------- antrean
static void flush_queue(void) {
  xSemaphoreTake(q_mx, portMAX_DELAY);
  int n = (int)uxQueueMessagesWaiting(q);
  xQueueReset(q);
  __atomic_fetch_sub(&pending, n, __ATOMIC_SEQ_CST);
  xSemaphoreGive(q_mx);
}

bool motion_push(const mot_item_t* it) {
  bool ok = false;
  xSemaphoreTake(q_mx, portMAX_DELAY);
  if (xQueueSend(q, it, 0) == pdTRUE) { __atomic_fetch_add(&pending, 1, __ATOMIC_SEQ_CST); ok = true; }
  xSemaphoreGive(q_mx);
  return ok;
}
int motion_room(void) { return MOT_QLEN - (int)uxQueueMessagesWaiting(q); }
bool motion_idle(void) { return pending == 0; }
int motion_pending(void) { return pending; }
void motion_abort(void) { abort_seq++; flush_queue(); }
void motion_pos(int32_t* a, int32_t* b) { *a = posA; *b = posB; }
void motion_set_pos(int32_t a, int32_t b) { posA = a; posB = b; }
bool motion_pen(void) { return pen_dn; }

static const char* swname(uint32_t m) {
  return (m & 1) ? "X-kiri" : (m & 4) ? "X-kanan" : (m & 2) ? "Y-bawah" : (m & 8) ? "Y-atas" : "?";
}

static void go_rel(int32_t a, int32_t b) {
  mot_item_t m = {.op = OP_MOVE, .stop = 0, .fl = FL_NOGUARD, .a = a, .b = b, .v = CFG(hFast)};
  run_move(&m, abort_seq);
}

// Akhir kalibrasi: penanda = posisi saat saklar kiri/kanan/bawah/atas tertekan. Sisakan hOff di tiap sisi,
// pindah ke titik nol (kiri-bawah + hOff), nolkan, lalu ke tengah.
static void cal_finish(void) {
  float spmm = CFG(spmm), O = CFG(hOff) * spmm;
  float x0 = (marks[0][0] + marks[0][1]) * 0.5f, x1 = (marks[1][0] + marks[1][1]) * 0.5f;     // x = (A+B)/2
  float y0 = (marks[2][0] - marks[2][1]) * 0.5f, y1 = (marks[3][0] - marks[3][1]) * 0.5f;     // y = (A-B)/2
  float w = fabsf(x1 - x0) / spmm - 2 * CFG(hOff), h = fabsf(y1 - y0) / spmm - 2 * CFG(hOff);
  if (w < 1 || h < 1) { io_log("error: kalibrasi gagal, jarak antar saklar terlalu kecil"); return; }
  settings_set(S_areaW, w);
  settings_set(S_areaH, h);
  io_log("kalibrasi: area %.1f x %.1f mm (kiri-kanan, bawah-atas); tekan SAVE untuk menyimpan", w, h);
  float cx = (posA + posB) * 0.5f, cy = (posA - posB) * 0.5f;                                    // posisi sekarang (langkah): di saklar atas
  float dx = (x0 + O) - cx, dy = (y0 + O) - cy;                                                  // menuju kiri-bawah + jarak aman
  go_rel((int32_t)lroundf(dx + dy), (int32_t)lroundf(dx - dy));
  posA = posB = 0;
  if (CFG(hCenter)) {
    float mx = w * spmm * 0.5f, my = h * spmm * 0.5f;
    go_rel((int32_t)lroundf(mx + my), (int32_t)lroundf(mx - my));
  }
}

static void process(const mot_item_t* it, uint32_t seq0) {
  if (abort_seq != seq0) { flush_queue(); return; }
  switch (it->op) {
    case OP_MOVE: {
      if (!driver_on && CFG(enMode) != 2) { motion_enable(true); esp_rom_delay_us((uint32_t)CFG(enDelayMs) * 1000u); }
      int r = run_move(it, seq0);
      if (r == MV_ABORT) { flush_queue(); }
      else if (r == MV_GUARD) {
        flush_queue();
        uint32_t g = guard_mask;
        if (pen_dn) pen_set(false);
        int32_t O = (int32_t)lroundf(CFG(hOff) * CFG(spmm));
        int32_t N = abs(it->a) > abs(it->b) ? abs(it->a) : abs(it->b);
        mot_item_t back = {.op = OP_MOVE, .stop = 0, .fl = FL_NOGUARD, .a = (int32_t)(-(int64_t)it->a * O / N), .b = (int32_t)(-(int64_t)it->b * O / N), .v = 0};
        run_move(&back, abort_seq);
        io_log("limit %s%s%s%s tertekan: berhenti dan mundur %g mm", (g & 1) ? "X-kiri " : "", (g & 4) ? "X-kanan " : "", (g & 2) ? "Y-bawah " : "", (g & 8) ? "Y-atas " : "", CFG(hOff));
      } else if (r == MV_WRONG) {
        flush_queue();
        io_log("error: ARAH TERBALIK - mencari saklar %s tapi %s yang tertekan. Berhenti. Tukar pin saklar (pinLX<->pinLX2 / pinLY<->pinLY2) lalu APPLY, atau balik motor (invA & invB), lalu ulangi.",
               swname(it->stop), swname(guard_mask));
      } else if (it->stop && r != MV_HIT) {
        flush_queue();
        io_log("error: saklar batas tidak tersentuh, homing/kalibrasi dibatalkan");
      }
      break;
    }
    case OP_ZERO: posA = 0; posB = 0; break;
    case OP_PEN_DN: pen_set(true); break;
    case OP_PEN_UP: pen_set(false); break;
    case OP_HOME_DONE: io_log("home selesai: X0 Y0"); break;
    case OP_MARK: marks[it->fl & 3][0] = posA; marks[it->fl & 3][1] = posB; break;
    case OP_CAL_FINISH: cal_finish(); break;
  }
}

static void motion_task(void* arg) {
  for (;;) {
    uint32_t seq0 = abort_seq;
    mot_item_t it;
    TickType_t wait = portMAX_DELAY;
    if (driver_on && CFG(enMode) == 0 && CFG(idleS) > 0) wait = pdMS_TO_TICKS((uint32_t)(CFG(idleS) * 1000));
    if (xQueueReceive(q, &it, wait) != pdTRUE) { motion_enable(false); continue; }
    xEventGroupSetBits(ev, EV_BUSY);
    process(&it, seq0);
    __atomic_fetch_sub(&pending, 1, __ATOMIC_SEQ_CST);
    xEventGroupClearBits(ev, EV_BUSY);
  }
}

void motion_init(void) {
  q = xQueueCreate(MOT_QLEN, sizeof(mot_item_t));
  q_mx = xSemaphoreCreateMutex();
  ev = xEventGroupCreate();
  const char* e = motion_apply_pins();
  if (e) io_log("error: pin: %s", e);
  xTaskCreatePinnedToCore(motion_task, "motion", 4096, NULL, configMAX_PRIORITIES - 3, NULL, 1);
}

#include "cmd.h"
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "motion.h"
#include "settings.h"
#include "io.h"
#include "net.h"
#include "led.h"

#define FS_BASE "/spiffs"

static SemaphoreHandle_t cmd_mx;
static int32_t planA, planB;                 // posisi (langkah) setelah semua item antrean selesai
static bool planPen;

static volatile bool job_active, job_abort;
static char job_name[40];

// ---------------------------------------------------------------- util
static void out(sink_t* s, const char* fmt, ...) {
  char b[220];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof b - 2) n = sizeof b - 2;
  b[n++] = '\n';
  s->write(s, b, (size_t)n);
}
static int r_ok(sink_t* s) { out(s, "ok"); return CMD_OK; }
static int r_busy(sink_t* s) { out(s, "busy"); return CMD_RETRY; }
static int r_full(sink_t* s) { out(s, "full"); return CMD_RETRY; }
static int r_err(sink_t* s, const char* m) { out(s, "error: %s", m); return CMD_ERR; }

static bool num(const char** p, float* o) {
  while (isspace((unsigned char)**p)) (*p)++;
  char* e;
  float v = strtof(*p, &e);
  if (e == *p) return false;
  *p = e;
  *o = v;
  return true;
}
static bool name_ok(const char* n) {
  size_t l = strlen(n);
  return l > 0 && l <= 30 && !strchr(n, '/') && !strstr(n, "..") && !strchr(n, ' ');
}

// ---------------------------------------------------------------- perencanaan antrean
static void sync_plan(void) {
  if (motion_idle()) { motion_pos(&planA, &planB); planPen = motion_pen(); }
}
static void plan_xy(float* x, float* y) {
  float k = 1.0f / CFG(spmm);
  *x = (planA + planB) * 0.5f * k;
  *y = (planA - planB) * 0.5f * k;
}
static bool put_leg(int32_t a, int32_t b, float v, uint8_t stop, uint8_t fl) {
  if (!a && !b && !stop) return true;
  mot_item_t it = {.op = OP_MOVE, .stop = stop, .fl = fl, .a = a, .b = b, .v = v};
  if (!motion_push(&it)) return false;
  planA += a; planB += b;
  return true;
}
static bool put_op(uint8_t op) {
  mot_item_t it = {.op = op};
  if (!motion_push(&it)) return false;
  if (op == OP_PEN_DN) planPen = true; else if (op == OP_PEN_UP) planPen = false;
  return true;
}
static bool goto_mm(float x, float y, float v, uint8_t fl) {      // CoreXY: A = x + y, B = x - y
  int32_t ta = lroundf((x + y) * CFG(spmm)), tb = lroundf((x - y) * CFG(spmm));
  return put_leg(ta - planA, tb - planB, v, 0, fl);
}
static bool put_circle(float r) {                                   // mulai di titik kiri, pusat ke kanan, berlawanan jarum jam
  float x, y;
  plan_xy(&x, &y);
  float cx = x + r, cy = y;
  for (int k = 1; k <= 64; k++) {
    float th = (float)M_PI + 2.0f * (float)M_PI * k / 64.0f;
    if (!goto_mm(cx + r * cosf(th), cy + r * sinf(th), CFG(drawv), k == 1 ? FL_NO_DECEL : (k == 64 ? FL_NO_ACCEL : 3))) return false;
  }
  return true;
}
static bool put_box(float d) {
  float x, y;
  plan_xy(&x, &y);
  return put_op(OP_PEN_DN) && goto_mm(x + d, y, CFG(drawv), 0) && goto_mm(x + d, y + d, CFG(drawv), 0) &&
         goto_mm(x, y + d, CFG(drawv), 0) && goto_mm(x, y, CFG(drawv), 0) && put_op(OP_PEN_UP);
}

static bool put_mark(int id) {
  mot_item_t it = {.op = OP_MARK, .fl = (uint8_t)id};
  return motion_push(&it);
}

// HOME: cari saklar di sisi asal (hdx/hdy). Jika saklar kanan/atas terpasang dan arah = +, pakai saklar kanan/atas.
static bool start_home(void) {
  float spmm = CFG(spmm);
  int tx = CFG(hdx) < 0 ? -1 : 1, ty = CFG(hdy) < 0 ? -1 : 1;
  int32_t SX = lroundf(fmaxf(CFG(hSearch), CFG(areaW) + 100) * spmm);
  int32_t SY = lroundf(fmaxf(CFG(hSearch), CFG(areaH) + 100) * spmm);
  int32_t O = lroundf(CFG(hOff) * spmm);
  float vf = CFG(hFast);
  uint8_t mx = tx < 0 ? 1 : (motion_has_max() ? 4 : 1), my = ty < 0 ? 2 : (motion_has_max() ? 8 : 2);
  uint32_t lim = motion_limits();
  if (lim & mx) if (!put_leg(-tx * O, -tx * O, vf, 0, 0)) return false;       // sudah tertekan: mundur dulu
  if (lim & my) if (!put_leg(-ty * O, ty * O, vf, 0, 0)) return false;
  bool ok = put_leg(tx * SX, tx * SX, vf, mx, 0) && put_leg(-tx * O, -tx * O, vf, 0, FL_NOGUARD) &&   // sumbu X = A,B searah
            put_leg(ty * SY, -ty * SY, vf, my, 0) && put_leg(-ty * O, ty * O, vf, 0, FL_NOGUARD);      // sumbu Y = A,B berlawanan
  if (!ok || !put_op(OP_ZERO)) return false;
  planA = planB = 0;
  if (CFG(hCenter)) {
    int32_t cx = lroundf(CFG(areaW) * 0.5f * spmm), cy = lroundf(CFG(areaH) * 0.5f * spmm);
    if (!put_leg(-tx * cx, -tx * cx, vf, 0, 0) || !put_leg(-ty * cy, ty * cy, vf, 0, 0)) return false;
  }
  return put_op(OP_HOME_DONE);
}

// CAL: kiri -> (langsung berbalik) kanan -> bawah -> (langsung berbalik) atas.
// Begitu saklar tertekan, gantry langsung berbalik arah dan terus berjalan sampai saklar seberang tertekan (tanpa mundur-lalu-berhenti).
// Penanda diambil tepat saat saklar tertekan. Lebar/tinggi area = jarak antar penanda dikurangi 2x hOff (jarak aman tiap sisi).
static bool start_cal(void) {
  float spmm = CFG(spmm), vf = CFG(hFast);
  int32_t SX = lroundf(fmaxf(CFG(hSearch), CFG(areaW) + 100) * spmm);
  int32_t SY = lroundf(fmaxf(CFG(hSearch), CFG(areaH) + 100) * spmm);
  int32_t O = lroundf(CFG(hOff) * spmm);
  uint32_t lim = motion_limits();
  bool sep = motion_has_max();                               // 4 saklar terpisah, atau kiri+kanan / bawah+atas satu pin
  bool ok = put_op(OP_PEN_UP);
  if (lim & 1) ok = ok && put_leg(O, O, vf, 0, 0);          // saklar sedang tertekan sebelum mulai: menjauh dulu
  if (lim & 4) ok = ok && put_leg(-O, -O, vf, 0, 0);
  if (lim & 2) ok = ok && put_leg(O, -O, vf, 0, 0);
  if (lim & 8) ok = ok && put_leg(-O, O, vf, 0, 0);
  uint8_t xr = sep ? 4 : 1, yt = sep ? 8 : 2;                // saklar seberang: pin terpisah, atau pin yang sama (tekan kedua)
  uint8_t ra = sep ? 0 : FL_REARM;
  ok = ok && put_leg(-SX, -SX, vf, 1, 0) && put_mark(0)      // ke kiri sampai saklar kiri tertekan
          && put_leg(SX, SX, vf, xr, ra) && put_mark(1)      // langsung berbalik ke kanan sampai saklar kanan tertekan
          && put_leg(-SY, SY, vf, 2, 0) && put_mark(2)       // ke bawah sampai saklar bawah tertekan
          && put_leg(SY, -SY, vf, yt, ra) && put_mark(3)     // langsung berbalik ke atas sampai saklar atas tertekan
          && put_op(OP_CAL_FINISH);
  planA = planB = 0;
  return ok;
}

// ---------------------------------------------------------------- file / job
static void fspath(char* p, size_t n, const char* name) { snprintf(p, n, FS_BASE "/%s", name); }

static void null_write(sink_t* s, const char* l, size_t n) { (void)s; (void)l; (void)n; }

static void job_task(void* arg) {
  char path[64];
  fspath(path, sizeof path, job_name);
  FILE* f = fopen(path, "r");
  sink_t ns = {.write = null_write};
  char line[200];
  if (!f) { io_log("error: job %s tidak ada", job_name); job_active = false; vTaskDelete(NULL); }
  io_log("job mulai: %s", job_name);
  bool failed = false;
  while (!job_abort && fgets(line, sizeof line, f)) {
    char* p = line;
    while (isspace((unsigned char)*p)) p++;
    if (!*p || *p == ';' || *p == '#') continue;
    int r;
    for (;;) {
      r = cmd_exec(p, &ns);
      if (r != CMD_RETRY || job_abort) break;
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (r == CMD_ERR) { io_log("error: job gagal di baris: %s", p); failed = true; break; }
  }
  fclose(f);
  while (!job_abort && !motion_idle()) vTaskDelay(pdMS_TO_TICKS(50));
  io_log(job_abort ? "job dihentikan" : (failed ? "job berhenti karena error" : "job selesai"));
  job_active = false;
  vTaskDelete(NULL);
}
bool job_running(void) { return job_active; }
void job_stop(void) { if (job_active) job_abort = true; }
bool job_start(const char* name) {
  if (job_active || !name_ok(name)) return false;
  strlcpy(job_name, name, sizeof job_name);
  job_abort = false;
  job_active = true;
  if (xTaskCreatePinnedToCore(job_task, "job", 6144, NULL, 4, NULL, 0) != pdPASS) { job_active = false; return false; }
  return true;
}

// ---------------------------------------------------------------- bantuan
static void help(sink_t* s) {
  static const char* const L[] = {
    "GERAK (butuh diam): R/L/U/D mm | XY dx dy | SQ mm | BOX mm | CIRCLE r | BOXCIRCLE mm | HOME | CAL (kiri-kanan-bawah-atas, ukur area)",
    "STREAM: MV x y (mm absolut dari titik nol) | PU | PD | PEN UP|DOWN  (balasan ok / full)",
    "OTA info slot | REBOOT   STATUS: ST | POS | LIM | PINSTAT | SHOW | LIST | VER | STOP   KALIBRASI MANUAL: ZERO (posisi ini = 0,0) | SETPOS x y",
    "SETELAN: LIST | GET nama | SET nama nilai | nama nilai | V S A DV HV | HDIR x y | AREA w h | SVUS min max",
    "PIN: PINS [sa da sb db sv lx ly en] lalu APPLY | SAVE (ke NVS) | CLEAR (bawaan) | RESET",
    "SERVO: ANGLE n | EN 0|1",
    "FLASH: LS | CAT f | DEL f | RUN f | PUTF f ... ENDF | FSINFO | CFGDUMP (tulis config.job)",
    "JARINGAN: WIFI [ssid [pass]] | IP     Balasan akhir perintah: ok / busy / full / error: ...",
  };
  for (unsigned i = 0; i < sizeof L / sizeof *L; i++) out(s, "%s", L[i]);
}

// ---------------------------------------------------------------- perintah
static int exec_locked(char* buf, sink_t* s);

int cmd_exec(const char* line, sink_t* s) {
  char buf[200];
  strlcpy(buf, line, sizeof buf);
  xSemaphoreTake(cmd_mx, portMAX_DELAY);
  int r = exec_locked(buf, s);
  xSemaphoreGive(cmd_mx);
  return r;
}

void cmd_sink_close(sink_t* s) {
  if (s->up) { fclose(s->up); s->up = NULL; }
}

static int exec_locked(char* buf, sink_t* s) {
  char* l = buf;
  while (isspace((unsigned char)*l)) l++;
  size_t n = strlen(l);
  while (n && isspace((unsigned char)l[n - 1])) l[--n] = 0;

  if (s->up) {                                                      // mode unggah file
    if (!strcasecmp(l, "ENDF")) { fclose(s->up); s->up = NULL; return r_ok(s); }
    fputs(l, s->up); fputc('\n', s->up);
    out(s, "+");
    return CMD_OK;
  }
  if (!n || *l == ';' || *l == '#') return CMD_OK;

  char* r = l;
  while (*r && !isspace((unsigned char)*r)) r++;
  if (*r) { *r++ = 0; while (isspace((unsigned char)*r)) r++; }
  char C[24];
  size_t ci = 0;
  for (; l[ci] && ci < sizeof C - 1; ci++) C[ci] = (char)toupper((unsigned char)l[ci]);
  C[ci] = 0;
  const char* q = r;
  float f[3] = {0, 0, 0};
  bool has1 = num(&q, &f[0]);

  if (!strcmp(C, "HELP")) { help(s); return r_ok(s); }
  if (!strcmp(C, "VER")) { out(s, "VER esp-plotter 1.0 (ESP-IDF %s)", esp_get_idf_version()); return r_ok(s); }
  if (!strcmp(C, "OTA")) {                 // info slot firmware; OTA sendiri lewat HTTP POST /ota
    const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* nxt = esp_ota_get_next_update_partition(NULL);
    out(s, "OTA jalan=%s sisa=%s ukuran_slot=%u", run ? run->label : "?", nxt ? nxt->label : "?", nxt ? (unsigned)nxt->size : 0u);
    return r_ok(s);
  }
  if (!strcmp(C, "REBOOT")) { out(s, "restart"); r_ok(s); vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); return 0; }
  if (!strcmp(C, "STOP")) {
    job_stop();
    motion_abort();
    for (int i = 0; i < 60 && !motion_idle(); i++) vTaskDelay(pdMS_TO_TICKS(5));
    sync_plan();
    return r_ok(s);
  }
  if (!strcmp(C, "ST") || !strcmp(C, "POS")) {
    int32_t a, b;
    motion_pos(&a, &b);
    float k = 1.0f / CFG(spmm);
    uint32_t lm = motion_limits();
    out(s, "%s q=%d busy=%d x=%.3f y=%.3f A=%ld B=%ld pen=%d lim=%d%d%d%d job=%d", C, motion_pending(), motion_idle() ? 0 : 1,
        (a + b) * 0.5f * k, (a - b) * 0.5f * k, (long)a, (long)b, motion_pen(), (int)(lm & 1), (int)((lm >> 1) & 1), (int)((lm >> 2) & 1), (int)((lm >> 3) & 1), job_active);
    return r_ok(s);
  }
  if (!strcmp(C, "LIM")) { uint32_t lm = motion_limits(); out(s, "LIM X=%d Y=%d X2=%d Y2=%d", (int)(lm & 1), (int)((lm >> 1) & 1), (int)((lm >> 2) & 1), (int)((lm >> 3) & 1)); return r_ok(s); }
  // ---- kalibrasi manual: tetapkan koordinat posisi gantry sekarang (tanpa menggerakkan motor)
  if (!strcmp(C, "SETPOS") || !strcmp(C, "ZERO")) {
    float y = 0;
    if (C[0] == 'S' && (!has1 || !num(&q, &y))) return r_err(s, "SETPOS x y  (mm, koordinat posisi sekarang)");
    if (!motion_idle()) return r_busy(s);
    float x = C[0] == 'S' ? f[0] : 0;
    motion_set_pos((int32_t)lroundf((x + y) * CFG(spmm)), (int32_t)lroundf((x - y) * CFG(spmm)));
    sync_plan();
    return r_ok(s);
  }
  if (!strcmp(C, "PINSTAT")) {   // diagnosis saklar: level mentah, apakah dianggap tertekan, dan polaritas
    static const char* const nm[4] = {"X-kiri", "Y-bawah", "X-kanan", "Y-atas"};
    uint32_t lm = motion_limits();
    out(s, "limActiveLow=%g (1: tertekan = LOW)", CFG(limActiveLow));
    for (int i = 0; i < 4; i++)
      out(s, "%-8s GPIO%-3d level=%d  -> %s", nm[i], motion_sw_pin(i), motion_raw_level(i), (lm >> i) & 1 ? "TERTEKAN" : "bebas");
    return r_ok(s);
  }
  if (!strcmp(C, "LIST")) {
    for (int i = 0; i < S_COUNT; i++) {
      const setting_t* t = settings_info(i);
      out(s, "S %s %g %g %g %s", t->name, g_cfg[i], t->min, t->max, t->desc);
    }
    return r_ok(s);
  }
  if (!strcmp(C, "SHOW")) {
    out(s, "spmm=%g vmax=%g vstart=%g accel=%g drawv=%g area=%gx%g svUp=%g svDn=%g", CFG(spmm), CFG(vmax), CFG(vstart), CFG(accel), CFG(drawv),
        CFG(areaW), CFG(areaH), CFG(svUp), CFG(svDn));
    out(s, "pins SA=%g DA=%g SB=%g DB=%g SV=%g LX=%g LY=%g EN=%g", CFG(pinSA), CFG(pinDA), CFG(pinSB), CFG(pinDB), CFG(pinSV), CFG(pinLX), CFG(pinLY), CFG(pinEN));
    return r_ok(s);
  }
  if (!strcmp(C, "GET") || !strcmp(C, "SET")) {
    char nm[32];
    int k = 0;
    while (*q && !isspace((unsigned char)*q) && k < 31) nm[k++] = *q++;
    nm[k] = 0;
    int i = settings_find(nm);
    if (i < 0) return r_err(s, "setelan tidak dikenal");
    if (C[0] == 'G') { out(s, "%s=%g", nm, g_cfg[i]); return r_ok(s); }
    float v;
    if (!num(&q, &v)) return r_err(s, "nilai?");
    settings_set(i, v);
    return r_ok(s);
  }
  if (!strcmp(C, "SAVE") || !strcmp(C, "CLEAR")) {
    if (!motion_idle()) return r_busy(s);
    if (C[0] == 'S') return settings_save() ? r_ok(s) : r_err(s, "gagal menulis NVS");
    settings_reset_defaults(true);
    return r_ok(s);
  }
  if (!strcmp(C, "RESET")) { out(s, "ok"); vTaskDelay(pdMS_TO_TICKS(100)); esp_restart(); return CMD_OK; }
  if (!strcmp(C, "APPLY")) {
    const char* e = motion_apply_pins();
    if (!e) e = led_apply();
    if (e) return !strcmp(e, "busy") ? r_busy(s) : r_err(s, e);
    return r_ok(s);
  }
  if (!strcmp(C, "PINS")) {
    static const int idx[8] = {S_pinSA, S_pinDA, S_pinSB, S_pinDB, S_pinSV, S_pinLX, S_pinLY, S_pinEN};
    if (has1) {
      float v[8] = {f[0]};
      for (int i = 1; i < 8; i++) if (!num(&q, &v[i])) return r_err(s, "PINS sa da sb db sv lx ly en");
      for (int i = 0; i < 8; i++) settings_set(idx[i], v[i]);
      out(s, "ok (jalankan APPLY)");
      return CMD_OK;
    }
    out(s, "PINS %g %g %g %g %g %g %g %g (sa da sb db sv lx ly en)", CFG(pinSA), CFG(pinDA), CFG(pinSB), CFG(pinDB), CFG(pinSV), CFG(pinLX), CFG(pinLY), CFG(pinEN));
    return r_ok(s);
  }
  if (!strcmp(C, "EN")) { if (!has1) return r_err(s, "EN 0|1"); motion_enable(f[0] != 0); return r_ok(s); }
  if (!strcmp(C, "ANGLE")) { if (!motion_idle()) return r_busy(s); if (!has1) return r_err(s, "ANGLE n"); motion_servo_angle(f[0]); return r_ok(s); }
  if (!strcmp(C, "HDIR")) { float y; if (!has1 || !num(&q, &y)) return r_err(s, "HDIR x y"); CFG(hdx) = f[0] < 0 ? -1 : 1; CFG(hdy) = y < 0 ? -1 : 1; return r_ok(s); }
  if (!strcmp(C, "AREA")) { float h; if (!has1) return r_err(s, "AREA w h"); if (!num(&q, &h)) h = f[0]; settings_set(S_areaW, f[0]); settings_set(S_areaH, h); return r_ok(s); }
  if (!strcmp(C, "SVUS")) { float mx; if (!has1 || !num(&q, &mx)) return r_err(s, "SVUS min max"); settings_set(S_svMinUs, f[0]); settings_set(S_svMaxUs, mx); return r_ok(s); }

  // ---- stream (boleh ditambah saat sedang bergerak)
  if (!strcmp(C, "MV") || !strcmp(C, "PU") || !strcmp(C, "PD")) {
    sync_plan();
    if (motion_room() < 4) return r_full(s);
    if (!strcmp(C, "PU")) { if (planPen) put_op(OP_PEN_UP); }
    else if (!strcmp(C, "PD")) { if (!planPen) put_op(OP_PEN_DN); }
    else {
      float y;
      if (!has1 || !num(&q, &y)) return r_err(s, "MV x y");
      if (planPen) goto_mm(f[0], y, CFG(drawv), 3);                // menulis: kecepatan tetap tanpa rem antar segmen
      else goto_mm(f[0], y, 0, 0);                                 // pindah pena naik: trapesium di kecepatan maks
    }
    return r_ok(s);
  }
  if (!strcmp(C, "PEN")) {
    if (!strncasecmp(r, "UP", 2)) { sync_plan(); if (motion_room() < 2) return r_full(s); put_op(OP_PEN_UP); }
    else if (!strncasecmp(r, "DO", 2) || !strncasecmp(r, "DN", 2)) { sync_plan(); if (motion_room() < 2) return r_full(s); put_op(OP_PEN_DN); }
    else return r_err(s, "PEN UP atau PEN DOWN");
    return r_ok(s);
  }

  // ---- kalibrasi area otomatis (4 saklar)
  if (!strcmp(C, "CAL")) {
    if (!motion_idle()) return r_busy(s);
    if (!motion_has_max() && (motion_limits() & 3)) return r_err(s, "saklar sedang tertekan: geser gantry ke tengah dulu (jog), lalu CAL");
    sync_plan();
    if (motion_room() < 40) return r_full(s);
    return start_cal() ? r_ok(s) : r_full(s);
  }

  // ---- gerak langsung
  if (!strcmp(C, "R") || !strcmp(C, "L") || !strcmp(C, "U") || !strcmp(C, "D") || !strcmp(C, "XY") || !strcmp(C, "SQ") || !strcmp(C, "BOX") ||
      !strcmp(C, "CIRCLE") || !strcmp(C, "BOXCIRCLE") || !strcmp(C, "HOME")) {
    if (!motion_idle()) return r_busy(s);
    sync_plan();
    if (motion_room() < 120) return r_full(s);
    float x, y;
    plan_xy(&x, &y);
    float d = f[0], v = 0;
    if (strcmp(C, "HOME") && !has1) return r_err(s, "butuh angka (mm)");
    bool ok = true;
    if (!strcmp(C, "R")) ok = goto_mm(x + d, y, v, 0);
    else if (!strcmp(C, "L")) ok = goto_mm(x - d, y, v, 0);
    else if (!strcmp(C, "U")) ok = goto_mm(x, y + d, v, 0);
    else if (!strcmp(C, "D")) ok = goto_mm(x, y - d, v, 0);
    else if (!strcmp(C, "XY")) { float dy; if (!num(&q, &dy)) return r_err(s, "XY dx dy"); ok = goto_mm(x + d, y + dy, v, 0); }
    else if (!strcmp(C, "SQ")) ok = goto_mm(x + d, y, v, 0) && goto_mm(x + d, y + d, v, 0) && goto_mm(x, y + d, v, 0) && goto_mm(x, y, v, 0);
    else if (!strcmp(C, "BOX")) ok = put_box(d);
    else if (!strcmp(C, "CIRCLE")) ok = put_op(OP_PEN_DN) && put_circle(d) && put_op(OP_PEN_UP);
    else if (!strcmp(C, "BOXCIRCLE")) {
      ok = put_box(d) && goto_mm(x, y + d / 2, v, 0) && put_op(OP_PEN_DN) && put_circle(d / 2) && put_op(OP_PEN_UP) && goto_mm(x, y, v, 0);
    } else ok = start_home();
    return ok ? r_ok(s) : r_full(s);
  }

  // ---- flash
  if (!strcmp(C, "LS")) {
    DIR* dd = opendir(FS_BASE);
    if (!dd) return r_err(s, "flash tidak terpasang");
    struct dirent* e;
    while ((e = readdir(dd))) {
      char p[300];
      struct stat st = {0};
      fspath(p, sizeof p, e->d_name);
      stat(p, &st);
      out(s, "F %s %ld", e->d_name, (long)st.st_size);
    }
    closedir(dd);
    return r_ok(s);
  }
  if (!strcmp(C, "FSINFO")) {
    size_t tot = 0, used = 0;
    esp_spiffs_info("storage", &tot, &used);
    out(s, "FS total=%u used=%u", (unsigned)tot, (unsigned)used);
    return r_ok(s);
  }
  if (!strcmp(C, "CAT") || !strcmp(C, "DEL") || !strcmp(C, "RUN") || !strcmp(C, "PUTF")) {
    if (!name_ok(r)) return r_err(s, "nama file tidak valid");
    char p[64];
    fspath(p, sizeof p, r);
    if (!strcmp(C, "DEL")) return remove(p) == 0 ? r_ok(s) : r_err(s, "file tidak ada");
    if (!strcmp(C, "RUN")) {
      struct stat st;
      if (stat(p, &st)) return r_err(s, "file tidak ada");
      return job_start(r) ? r_ok(s) : r_busy(s);
    }
    if (!strcmp(C, "PUTF")) {
      s->up = fopen(p, "w");
      return s->up ? r_ok(s) : r_err(s, "gagal membuat file");
    }
    FILE* fp = fopen(p, "r");
    if (!fp) return r_err(s, "file tidak ada");
    char ln[200];
    while (fgets(ln, sizeof ln, fp)) { size_t k = strlen(ln); while (k && (ln[k - 1] == '\n' || ln[k - 1] == '\r')) ln[--k] = 0; out(s, "| %s", ln); }
    fclose(fp);
    return r_ok(s);
  }
  if (!strcmp(C, "CFGDUMP")) {
    FILE* fp = fopen(FS_BASE "/config.job", "w");
    if (!fp) return r_err(s, "gagal menulis");
    for (int i = 0; i < S_COUNT; i++) fprintf(fp, "%s %g\n", settings_info(i)->name, g_cfg[i]);
    fclose(fp);
    return r_ok(s);
  }

  // ---- jaringan
  if (!strcmp(C, "IP") || (!strcmp(C, "WIFI") && !*r)) {
    char b[160];
    net_info(b, sizeof b);
    out(s, "%s", b);
    return r_ok(s);
  }
  if (!strcmp(C, "WIFI")) {
    char ssid[40] = "", pass[70] = "";
    const char* p = r;
    size_t k = 0;
    while (*p && !isspace((unsigned char)*p) && k < sizeof ssid - 1) ssid[k++] = *p++;
    ssid[k] = 0;
    while (isspace((unsigned char)*p)) p++;
    strlcpy(pass, p, sizeof pass);
    if (!strcmp(ssid, "-")) ssid[0] = pass[0] = 0;                  // WIFI -  -> hapus (mode AP)
    wifi_cred_save(ssid, pass);
    net_wifi_reconfigure();
    out(s, "ok wifi diset, IP baru akan diumumkan (! wifi ...)");
    return CMD_OK;
  }

  // ---- setelan: "nama nilai" atau alias pendek
  const char* nm = C;
  if (!strcmp(C, "V")) nm = "vmax";
  else if (!strcmp(C, "S")) nm = "vstart";
  else if (!strcmp(C, "A")) nm = "accel";
  else if (!strcmp(C, "DV")) nm = "drawv";
  else if (!strcmp(C, "HV")) nm = "hFast";
  int i = settings_find(nm);
  if (i >= 0) {
    if (!has1) { out(s, "%s=%g", settings_info(i)->name, g_cfg[i]); return r_ok(s); }
    settings_set(i, f[0]);
    if (settings_is_pin(i)) { out(s, "ok (jalankan APPLY)"); return CMD_OK; }
    return r_ok(s);
  }
  return r_err(s, "perintah tidak dikenal, ketik HELP");
}

void cmd_init(void) { cmd_mx = xSemaphoreCreateMutex(); }

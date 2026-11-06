// Kontrol motor CoreXY di Raspberry Pi Pico (RP2040), Pico C SDK murni, TANPA float sama sekali.
// Core 1: pulsa STEP (busy-wait, integer). Core 0: USB serial, antrean gerak, homing, servo.
// Angka perintah diparse sebagai bilangan tetap (1/1000), mis. "12.5" -> 12500.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/flash.h"

#define SYS_KHZ 200000                                   // overclock 200 MHz (bawaan 125); turunkan ke 133000/125000 kalau tidak stabil

// --- pin (ubah sesuai kabel) ---
#define PIN_SA 2     // STEP motor A
#define PIN_DA 3     // DIR  motor A
#define PIN_SB 4     // STEP motor B
#define PIN_DB 5     // DIR  motor B
#define PIN_SV 6     // servo pena
#define PIN_LX 7     // tombol batas X (ke GND saat ditekan)
#define PIN_LY 8     // tombol batas Y (ke GND saat ditekan)
#define PIN_EN 9     // ENABLE driver (aktif LOW untuk A4988/HR4988)

static volatile int32_t spmm = 80;                       // langkah/mm (1/16 microstep, GT2 puli 20 gigi)
static volatile uint32_t vmax = 80, vstart = 80;         // langkah/detik (80 = 1 mm/s)
static volatile uint32_t accel = 4000;                   // langkah/detik^2 (= 50 mm/s^2)
static volatile bool invA = false, invB = false;
static volatile int pulseUs = 4;

// homing (nilai dalam milli-mm atau milli-mm/s)
static int32_t hFast = 0, hSlow = 0;                     // 0 = otomatis: cepat = vmax, pelan = vstart (profil di halaman)
static int32_t hBack = 3000, hOff = 10000, hSearch = 300000;
static int32_t areaW = 100000, areaH = 100000;           // area kerja (milli-mm), untuk gerak ke tengah
static bool hCenter = true;                              // setelah homing pindah ke tengah area
static int hdx = -1, hdy = -1;                           // arah menuju tombol: -1 = ke X/Y minimum

// servo
static int svUp = 0, svDn = 90, svMin = 500, svMax = 2500, svNow = -1;

static volatile bool busy = false, abortReq = false, go = false, lastHit = false;
static volatile int32_t reqA = 0, reqB = 0;
static volatile uint32_t reqV = 0;
static volatile int reqStop = -1;
static volatile uint8_t reqFl = 0;                       // bit0: tanpa akselerasi awal, bit1: tanpa perlambatan akhir (sambung antar segmen)
static volatile int32_t posA = 0, posB = 0;

static uint32_t isqrt64(uint64_t x) {
  uint64_t r = 0, bit = 1ULL << 62;
  while (bit > x) bit >>= 2;
  while (bit) {
    if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
    bit >>= 2;
  }
  return (uint32_t)r;
}

static inline bool pressed(int pin) { return !gpio_get(pin); }

#define LIM_MASK ((1u << PIN_LX) | (1u << PIN_LY))
#define STOP_ANY 99                                      // stop_pin khusus: berhenti begitu tombol X atau Y mana pun tertekan
#define FL_NOGUARD 4                                    // bit2 fl: abaikan pengawal tombol (gerak mundur dari tombol)
static volatile uint32_t guardTrip = 0;                  // bit pin tombol yang tertekan saat gerak biasa; diurus core 0

// tombol di mask tertekan terus-menerus >= 150 us (tahan getar); since = waktu pertama terlihat
static inline bool lim_hit(uint32_t mask, uint32_t* since) {
  if (!(~gpio_get_all() & mask)) { *since = 0; return false; }
  uint32_t now = time_us_32() | 1u;
  if (!*since) { *since = now; return false; }
  return (now - *since) >= 150;
}

// satu gerakan lurus. stop_pin >= 0: berhenti begitu tombol itu tertekan (return true).
// stop_pin < 0: pengawal; tombol yang BARU tertekan selama gerak menghentikan gerakan dan mengisi guardTrip.
static bool run_move(int32_t sa, int32_t sb, uint32_t vover, int stop_pin, uint8_t fl) {
  int32_t aa = abs(sa), bb = abs(sb), N = aa > bb ? aa : bb;
  if (!N) return false;
  uint32_t mask = stop_pin == STOP_ANY ? LIM_MASK : stop_pin >= 0 ? (1u << stop_pin) : ((fl & FL_NOGUARD) ? 0 : (LIM_MASK & gpio_get_all()));   // tombol yang sudah tertekan di awal diabaikan
  uint32_t since = 0;
  bool masterA = aa >= bb;
  int32_t slv = masterA ? bb : aa;
  gpio_put(PIN_DA, (sa > 0) != invA);
  gpio_put(PIN_DB, (sb > 0) != invB);
  busy_wait_us_32(10);
  uint32_t vm = vover ? vover : vmax, ac = accel;
  uint32_t vs = vstart < vm ? vstart : vm;
  uint64_t v0sq = (uint64_t)vs * vs, vmsq = (uint64_t)vm * vm;
  int32_t accSteps = (int32_t)((vmsq - v0sq) / (2ULL * ac));
  if (accSteps * 2 > N) accSteps = N / 2;
  int32_t err = N / 2;
  uint32_t t = time_us_32();
  for (int32_t i = 0; i < N && !abortReq; i++) {
    if (mask && lim_hit(mask, &since)) goto tripped;
    uint64_t v2;
    if (!(fl & 1) && i < accSteps) v2 = v0sq + 2ULL * ac * i;
    else if (!(fl & 2) && i >= N - accSteps) v2 = v0sq + 2ULL * ac * (N - 1 - i);
    else v2 = vmsq;
    uint32_t v = isqrt64(v2);
    if (v < vs) v = vs;
    err += slv;
    bool st = false;
    if (err >= N) { err -= N; st = true; }
    bool sA = masterA ? true : st, sB = masterA ? st : true;
    if (sA) gpio_put(PIN_SA, 1);
    if (sB) gpio_put(PIN_SB, 1);
    busy_wait_us_32(pulseUs);
    gpio_put(PIN_SA, 0); gpio_put(PIN_SB, 0);
    if (sA) posA += sa > 0 ? 1 : -1;
    if (sB) posB += sb > 0 ? 1 : -1;
    t += 1000000UL / v;
    while ((int32_t)(time_us_32() - t) < 0) { if (mask && lim_hit(mask, &since)) goto tripped; }   // tombol dicek juga di sela langkah
  }
  return false;
tripped:
  if (stop_pin >= 0) return true;
  { uint32_t p = ~gpio_get_all() & mask; guardTrip = p ? p : mask; }
  return false;
}

static void core1_main(void) {
  multicore_lockout_victim_init();                       // core 0 boleh menghentikan core 1 sebentar saat menulis flash
  for (;;) {
    if (go) {
      __dmb();
      int32_t a = reqA, b = reqB; uint32_t v = reqV; int sp = reqStop; uint8_t fl = reqFl;
      go = false;
      bool hit = run_move(a, b, v, sp, fl);
      lastHit = hit;
      __dmb();
      busy = false;
    }
  }
}

// ---------------------------------------------------------------- pengaturan di flash (sektor terakhir)
#define CFG_MAGIC 0x50434647u                            // "PCFG"
#define CFG_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
typedef struct {
  uint32_t magic;
  int32_t spmm; uint32_t vmax, vstart, accel;
  int32_t invA, invB, pulseUs;
  int32_t hFast, hSlow, hBack, hOff, hSearch, areaW, areaH, hCenter, hdx, hdy;
  int32_t svUp, svDn, svMin, svMax;
  uint32_t sum;
} Cfg;
static bool cfgSaved = false;

static uint32_t cfg_sum(const Cfg* c) {
  const uint32_t* p = (const uint32_t*)c;
  uint32_t s = 0x9e3779b9u;
  for (size_t i = 0; i < offsetof(Cfg, sum) / 4; i++) s = s * 31u + p[i];
  return s;
}
static void cfg_load(void) {                             // dipanggil sekali saat boot, sebelum servo_init
  const Cfg* c = (const Cfg*)(XIP_BASE + CFG_OFFSET);
  if (c->magic != CFG_MAGIC || c->sum != cfg_sum(c) || c->spmm < 1) return;
  spmm = c->spmm; vmax = c->vmax; vstart = c->vstart; accel = c->accel;
  invA = c->invA != 0; invB = c->invB != 0; pulseUs = c->pulseUs;
  hFast = c->hFast; hSlow = c->hSlow; hBack = c->hBack; hOff = c->hOff; hSearch = c->hSearch;
  areaW = c->areaW; areaH = c->areaH; hCenter = c->hCenter != 0; hdx = c->hdx; hdy = c->hdy;
  svUp = c->svUp; svDn = c->svDn; svMin = c->svMin; svMax = c->svMax;
  cfgSaved = true;
}
static void flash_write_cfg(const uint8_t* page) {       // page = NULL: hanya hapus
  multicore_lockout_start_blocking();
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(CFG_OFFSET, FLASH_SECTOR_SIZE);
  if (page) flash_range_program(CFG_OFFSET, page, FLASH_PAGE_SIZE);
  restore_interrupts(ints);
  multicore_lockout_end_blocking();
}
static void cfg_save(void) {
  Cfg c = { CFG_MAGIC, spmm, vmax, vstart, accel, invA, invB, pulseUs,
            hFast, hSlow, hBack, hOff, hSearch, areaW, areaH, hCenter, hdx, hdy, svUp, svDn, svMin, svMax, 0 };
  c.sum = cfg_sum(&c);
  uint8_t page[FLASH_PAGE_SIZE];
  memset(page, 0xFF, sizeof page);
  memcpy(page, &c, sizeof c);
  flash_write_cfg(page);
  cfgSaved = true;
}

// ---------------------------------------------------------------- servo (PWM 50 Hz, 1 us per hitungan)
static void servo_us(int us) {
  uint sl = pwm_gpio_to_slice_num(PIN_SV);
  pwm_set_chan_level(sl, pwm_gpio_to_channel(PIN_SV), (uint16_t)us);
}
static void servo_angle(int deg) {
  if (deg < 0) deg = 0;
  if (deg > 180) deg = 180;
  svNow = deg;
  servo_us(svMin + (svMax - svMin) * deg / 180);
}
static void servo_init(void) {
  gpio_set_function(PIN_SV, GPIO_FUNC_PWM);
  uint sl = pwm_gpio_to_slice_num(PIN_SV);
  pwm_set_clkdiv_int_frac(sl, clock_get_hz(clk_sys) / 1000000, 0);   // jam sistem / N = 1 MHz, apa pun overclock-nya
  pwm_set_wrap(sl, 19999);                   // 20 ms
  servo_angle(svUp);
  pwm_set_enabled(sl, true);
}

// ---------------------------------------------------------------- core 0: antrean
// op: 0 gerak, 1 nolkan posisi, 2 pena turun, 3 pena naik
typedef struct { uint8_t op; int32_t a, b; uint32_t v; int8_t stop; uint8_t fl; } Leg;
#define MAXLEG 100
static Leg seq[MAXLEG];
static int seqLen = 0, seqIdx = 0;
static bool curNeed = false, homing = false;
static uint32_t waitUntil = 0;
#define PEN_MS 300

static int32_t planA = 0, planB = 0;                     // posisi (langkah) setelah semua gerak di antrean selesai
static bool planPen = false;                             // pena turun setelah antrean selesai
static uint32_t drawV = 0;                               // kecepatan menggambar (langkah/s), 0 = vstart

static bool seq_room(int n) {                            // pastikan n slot kosong; geser antrean yang sudah terpakai
  if (seqLen + n <= MAXLEG) return true;
  if (seqIdx > 0) { memmove(seq, seq + seqIdx, (size_t)(seqLen - seqIdx) * sizeof(Leg)); seqLen -= seqIdx; seqIdx = 0; }
  return seqLen + n <= MAXLEG;
}
static void add_op(uint8_t op) {
  if (!seq_room(1)) return;
  seq[seqLen].op = op; seqLen++;
  if (op == 2) planPen = true; else if (op == 3) planPen = false;
}
static void add_leg_fl(int32_t a, int32_t b, uint32_t v, int stop, uint8_t fl) {
  if (!seq_room(1)) return;
  Leg* l = &seq[seqLen++]; l->op = 0; l->a = a; l->b = b; l->v = v; l->stop = (int8_t)stop; l->fl = fl;
  planA += a; planB += b;
}
static void add_leg(int32_t a, int32_t b, uint32_t v, int stop) { add_leg_fl(a, b, v, stop, 0); }

// lingkaran: 64 segmen dari tabel cos(2*pi*i/64)*1000 (integer). Mulai di titik paling kiri, pusat R ke kanan, berlawanan jarum jam.
static const int16_t COS64[64] = {1000, 995, 981, 957, 924, 882, 831, 773, 707, 634, 556, 471, 383, 290, 195, 98, 0, -98, -195, -290, -383, -471, -556, -634, -707, -773, -831, -882, -924, -957, -981, -995, -1000, -995, -981, -957, -924, -882, -831, -773, -707, -634, -556, -471, -383, -290, -195, -98, 0, 98, 195, 290, 383, 471, 556, 634, 707, 773, 831, 882, 924, 957, 981, 995};
static int32_t rdiv1000(int64_t p) { return (int32_t)((p + (p >= 0 ? 500 : -500)) / 1000); }
static void add_circle(int32_t R) {                  // R dalam langkah
  if (R < 1) return;
  int32_t pa = 0, pb = 0;
  for (int k = 1; k <= 64; k++) {
    int32_t c = COS64[(k + 32) & 63], s = COS64[(k + 16) & 63];        // cos dan sin dari sudut (pi + 2*pi*k/64)
    int32_t dx = rdiv1000((int64_t)R * (c + 1000)), dy = rdiv1000((int64_t)R * s);
    int32_t A = dx + dy, B = dx - dy;                // CoreXY
    add_leg_fl(A - pa, B - pb, 0, -1, k == 1 ? 2 : (k == 64 ? 1 : 3));
    pa = A; pb = B;
  }
}
static int32_t to_steps(int32_t milli) {             // milli-mm -> langkah (dibulatkan, integer)
  int64_t p = (int64_t)milli * spmm;
  return (int32_t)((p + (p >= 0 ? 500 : -500)) / 1000);
}
static uint32_t to_sps(int32_t milli) {              // milli-mm/s -> langkah/detik, minimal 1
  int64_t p = (int64_t)milli * spmm / 1000;
  return p < 1 ? 1 : (uint32_t)p;
}
static void push_dir(char d, int32_t st) {           // CoreXY: kanan = A+,B+ ; atas = A+,B-
  if (d == 'R') add_leg(st, st, 0, -1);
  else if (d == 'L') add_leg(-st, -st, 0, -1);
  else if (d == 'U') add_leg(st, -st, 0, -1);
  else if (d == 'D') add_leg(-st, st, 0, -1);
}
static void enable(bool on) { gpio_put(PIN_EN, on ? 0 : 1); }

static void pump(void) {
  if (busy || go) return;
  if (guardTrip) {                                       // tombol batas tertekan saat gerak biasa: batalkan antrean, mundur
    uint32_t g = guardTrip; guardTrip = 0;
    bool pen = planPen;
    seqLen = seqIdx = 0; curNeed = false; homing = false; planPen = false;
    if (pen) { servo_angle(svUp); waitUntil = time_us_32() + PEN_MS * 1000UL; }
    int32_t O = to_steps(hOff);
    int32_t ra = reqA, rb = reqB, N = abs(ra) > abs(rb) ? abs(ra) : abs(rb);   // gerakan yang baru terhenti
    planA = posA; planB = posB;
    if (N) add_leg_fl((int32_t)(-(int64_t)ra * O / N), (int32_t)(-(int64_t)rb * O / N), 0, -1, FL_NOGUARD);   // mundur persis berlawanan arah gerak
    printf("limit %s%s tertekan: berhenti dan mundur %ld mm\n", (g & (1u << PIN_LX)) ? "X" : "", (g & (1u << PIN_LY)) ? "Y" : "", (long)(hOff / 1000));
  }
  if ((int32_t)(time_us_32() - waitUntil) < 0) return;
  if (curNeed) {
    curNeed = false;
    if (!lastHit && !abortReq) {
      seqLen = seqIdx = 0; homing = false;
      puts("error: tombol batas tidak tersentuh, homing dibatalkan");
      return;
    }
  }
  while (seqIdx < seqLen && seq[seqIdx].op != 0) {
    uint8_t op = seq[seqIdx++].op;
    if (op == 1) { posA = 0; posB = 0; }
    else if (op == 2) { servo_angle(svDn); waitUntil = time_us_32() + PEN_MS * 1000UL; return; }
    else if (op == 3) { servo_angle(svUp); waitUntil = time_us_32() + PEN_MS * 1000UL; return; }
  }
  if (seqIdx < seqLen) {
    Leg* l = &seq[seqIdx++];
    reqA = l->a; reqB = l->b; reqV = l->v; reqStop = l->stop; reqFl = l->fl;
    curNeed = l->stop >= 0;
    busy = true; __dmb(); go = true;
  } else if (homing) { homing = false; puts("home selesai: X0 Y0"); }
}

static void start_home(void) {
  int32_t sx = hSearch > areaW + 100000 ? hSearch : areaW + 100000;     // jarak cari minimal selebar area + 100 mm
  int32_t sy = hSearch > areaH + 100000 ? hSearch : areaH + 100000;
  int32_t SX = to_steps(sx), SY = to_steps(sy), O = to_steps(hOff);
  uint32_t vf = hFast > 0 ? to_sps(hFast) : vmax;
  int tx = hdx, ty = hdy;
  // tombol sudah tertekan sebelum HOME: mundur dulu supaya tidak menempel
  if (pressed(PIN_LX)) add_leg(-tx * O, -tx * O, vf, -1);
  if (pressed(PIN_LY)) add_leg(-ty * O, ty * O, vf, -1);
  // X: dekati sampai tombol tertekan, langsung mundur O mm. CoreXY: gerak X = A,B searah.
  add_leg(tx * SX, tx * SX, vf, STOP_ANY);
  add_leg(-tx * O, -tx * O, vf, -1);
  // Y: gerak Y = A,B berlawanan
  add_leg(ty * SY, -ty * SY, vf, STOP_ANY);
  add_leg(-ty * O, ty * O, vf, -1);
  add_op(1);                                        // nol = O mm dari tombol
  if (hCenter) {                                    // ke tengah area: X dulu (kanan-kiri), lalu Y (atas-bawah)
    int32_t cx = to_steps(areaW / 2), cy = to_steps(areaH / 2);
    add_leg(-tx * cx, -tx * cx, vf, -1);
    add_leg(-ty * cy, ty * cy, vf, -1);
  }
  planA = planB = 0;
  if (hCenter) {
    int32_t cx = to_steps(areaW / 2), cy = to_steps(areaH / 2);
    planA = -tx * cx - ty * cy; planB = -tx * cx + ty * cy;
  }
  homing = true;
}

// ---------------------------------------------------------------- parsing tanpa float
static int32_t parse_milli(const char** pp) {        // "-12.345" -> -12345
  const char* p = *pp;
  while (*p == ' ') p++;
  bool neg = false;
  if (*p == '-') { neg = true; p++; } else if (*p == '+') p++;
  int32_t ip = 0;
  while (isdigit((unsigned char)*p)) ip = ip * 10 + (*p++ - '0');
  int32_t fp = 0, scale = 100;
  if (*p == '.') {
    p++;
    while (isdigit((unsigned char)*p)) { if (scale) { fp += (*p - '0') * scale; scale /= 10; } p++; }
  }
  *pp = p;
  int32_t v = ip * 1000 + fp;
  return neg ? -v : v;
}

static void help(void) {
  puts("R/L/U/D mm   kanan/kiri/atas/bawah     XY dx dy (mm)     SQ mm = kotak (tanpa pena)");
  puts("CIRCLE r  lingkaran jari-jari r (mulai di titik kiri, pusat ke kanan)   BOXCIRCLE mm  kotak + lingkaran di dalamnya");
  puts("MV x y  PU  PD  DV mm/s  ST   (streaming: MV absolut mm dari titik nol, dipakai halaman untuk SVG)");
  puts("BOX mm       gambar kotak: pena turun, kotak, pena naik");
  puts("HOME         kalibrasi otomatis: tombol X di GPIO7, Y di GPIO8, nol di titik itu");
  puts("HV fast slow (mm/s, 0 0 = ikut V dan S)  HSEARCH mm  HBACK mm  HOFF mm (jarak nol dari tombol)  HDIR x y (+1/-1)  LIM");
  puts("AREA w h (mm)  HCENTER 0|1  (setelah HOME gantry ke tengah area)");
  puts("PEN UP|DOWN  ANGLE n  SVUP n  SVDN n  SVUS min max");
  puts("V mm/s  S mm/s(awal)  A mm/s2  SPMM n  PULSE us  INVA 0|1  INVB 0|1  EN 0|1");
  puts("SAVE  simpan semua pengaturan ke flash (dimuat otomatis saat menyala)   CLEAR  hapus simpanan");
  puts("STOP  POS  SHOW");
}

static void handle(char* line) {
  while (isspace((unsigned char)*line)) line++;
  for (char* p = line; *p; p++) *p = (char)toupper((unsigned char)*p);
  size_t n = strlen(line);
  while (n && isspace((unsigned char)line[n - 1])) line[--n] = 0;
  if (!n) return;
  char* r = strchr(line, ' ');
  if (r) { *r++ = 0; while (isspace((unsigned char)*r)) r++; } else r = line + n;
  const char* c = line;
  const char* q = r;
  int32_t x = parse_milli(&q);                       // angka pertama (milli)

  if (!strcmp(c, "HELP")) { help(); return; }
  if (!strcmp(c, "STOP")) { abortReq = true; seqLen = seqIdx = 0; curNeed = false; homing = false; planPen = false; puts("ok stop"); return; }
  if (!strcmp(c, "ST")) { printf("ST q=%d busy=%d\n", seqLen - seqIdx, (busy || go) ? 1 : 0); return; }
  if (!strcmp(c, "DV")) { drawV = x > 0 ? to_sps(x) : 0; puts("ok"); return; }
  if (!strcmp(c, "POS")) { printf("A=%ld B=%ld langkah\n", (long)posA, (long)posB); return; }
  if (!strcmp(c, "LIM")) { printf("LIM X=%d Y=%d\n", pressed(PIN_LX), pressed(PIN_LY)); return; }
  if (!strcmp(c, "SHOW")) {
    printf("spmm=%ld vmax=%lu vstart=%lu accel=%lu invA=%d invB=%d svUp=%d svDn=%d hdir=%d,%d area=%ld,%ld hoff=%ld hcenter=%d cpu=%luMHz saved=%d\n", (long)spmm,
           (unsigned long)vmax, (unsigned long)vstart, (unsigned long)accel, invA, invB, svUp, svDn, hdx, hdy,
           (long)(areaW / 1000), (long)(areaH / 1000), (long)(hOff / 1000), hCenter,
           (unsigned long)(clock_get_hz(clk_sys) / 1000000), cfgSaved);
    return;
  }
  if (!strcmp(c, "SAVE") || !strcmp(c, "CLEAR")) {   // simpan / hapus pengaturan di flash (hanya saat diam)
    if (busy || go || seqIdx < seqLen) { puts("busy"); return; }
    if (c[0] == 'S') { cfg_save(); puts("ok tersimpan di flash"); }
    else { flash_write_cfg(NULL); cfgSaved = false; puts("ok simpanan dihapus"); }
    return;
  }
  // pengaturan (boleh kapan saja)
  if (!strcmp(c, "V")) { vmax = to_sps(x); puts("ok"); return; }
  if (!strcmp(c, "S")) { vstart = to_sps(x); puts("ok"); return; }
  if (!strcmp(c, "A")) { accel = to_sps(x); puts("ok"); return; }
  if (!strcmp(c, "SPMM")) { int32_t v = x / 1000; if (v < 1) v = 1; spmm = v; puts("ok"); return; }
  if (!strcmp(c, "PULSE")) { pulseUs = x / 1000; puts("ok"); return; }
  if (!strcmp(c, "INVA")) { invA = x != 0; puts("ok"); return; }
  if (!strcmp(c, "INVB")) { invB = x != 0; puts("ok"); return; }
  if (!strcmp(c, "EN")) { enable(x != 0); puts("ok"); return; }
  if (!strcmp(c, "HV")) { hFast = x; hSlow = parse_milli(&q); if (hSlow <= 0) hSlow = hFast; puts("ok"); return; }
  if (!strcmp(c, "HSEARCH")) { hSearch = x; puts("ok"); return; }
  if (!strcmp(c, "HBACK")) { hBack = x; puts("ok"); return; }
  if (!strcmp(c, "HOFF")) { hOff = x; puts("ok"); return; }
  if (!strcmp(c, "AREA")) { areaW = x; areaH = parse_milli(&q); if (areaH <= 0) areaH = areaW; puts("ok"); return; }
  if (!strcmp(c, "HCENTER")) { hCenter = x != 0; puts("ok"); return; }
  if (!strcmp(c, "HDIR")) { hdx = x < 0 ? -1 : 1; int32_t y = parse_milli(&q); hdy = y < 0 ? -1 : 1; puts("ok"); return; }
  if (!strcmp(c, "SVUP")) { svUp = x / 1000; puts("ok"); return; }
  if (!strcmp(c, "SVDN")) { svDn = x / 1000; puts("ok"); return; }
  if (!strcmp(c, "SVUS")) { svMin = x / 1000; svMax = parse_milli(&q) / 1000; puts("ok"); return; }
  if (!strcmp(c, "ANGLE")) { servo_angle(x / 1000); puts("ok"); return; }
  if (!strcmp(c, "PEN")) {
    if (!strncmp(r, "UP", 2)) servo_angle(svUp);
    else if (!strncmp(r, "DO", 2) || !strncmp(r, "DN", 2)) servo_angle(svDn);
    else { puts("error: PEN UP atau PEN DOWN"); return; }
    puts("ok"); return;
  }
  // streaming dari halaman: MV x y (mm absolut dari titik nol), PU/PD; boleh ditambah saat sedang bergerak
  if (!strcmp(c, "MV") || !strcmp(c, "PU") || !strcmp(c, "PD")) {
    if (abortReq && !busy && !go) abortReq = false;
    if (abortReq) { puts("stopped"); return; }
    if (!busy && !go && seqIdx >= seqLen) { seqLen = seqIdx = 0; planA = posA; planB = posB; }
    if (!seq_room(2)) { puts("full"); return; }
    if (!strcmp(c, "PU")) { if (planPen) add_op(3); }
    else if (!strcmp(c, "PD")) { if (!planPen) add_op(2); }
    else {
      int32_t y = parse_milli(&q);
      int32_t ta = to_steps(x + y), tb = to_steps(x - y);          // CoreXY: A = x + y, B = x - y
      uint32_t dv = drawV ? drawV : vstart;
      if (planPen) add_leg_fl(ta - planA, tb - planB, dv, -1, 3);  // menulis: kecepatan tetap, tanpa rem antar segmen
      else add_leg_fl(ta - planA, tb - planB, 0, -1, 0);           // pindah pena naik: trapesium di kecepatan maks
    }
    puts("ok");
    return;
  }
  // gerak
  if (busy || seqIdx < seqLen) { puts("busy"); return; }
  planA = posA; planB = posB;
  abortReq = false; seqLen = seqIdx = 0;
  int32_t st = to_steps(x);
  if (!strcmp(c, "R") || !strcmp(c, "L") || !strcmp(c, "U") || !strcmp(c, "D")) push_dir(c[0], st);
  else if (!strcmp(c, "SQ")) { push_dir('R', st); push_dir('U', st); push_dir('L', st); push_dir('D', st); }
  else if (!strcmp(c, "BOX")) {
    add_op(2); push_dir('R', st); push_dir('U', st); push_dir('L', st); push_dir('D', st); add_op(3);
  }
  else if (!strcmp(c, "CIRCLE")) { add_op(2); add_circle(st); add_op(3); }
  else if (!strcmp(c, "BOXCIRCLE")) {               // kotak sisi x, lalu lingkaran dalam yang menyinggung sisinya (diameter = x)
    add_op(2); push_dir('R', st); push_dir('U', st); push_dir('L', st); push_dir('D', st); add_op(3);
    push_dir('U', st / 2);                           // pena naik, ke titik kiri-tengah
    add_op(2); add_circle(st / 2); add_op(3);
    push_dir('D', st / 2);                           // kembali ke sudut awal
  }
  else if (!strcmp(c, "XY")) {
    int32_t dy = parse_milli(&q);
    add_leg(to_steps(x + dy), to_steps(x - dy), 0, -1);
  }
  else if (!strcmp(c, "HOME")) start_home();
  else { puts("error: HELP"); return; }
  puts("ok");
}

static void pin_out(uint pin, bool v) { gpio_init(pin); gpio_set_dir(pin, GPIO_OUT); gpio_put(pin, v); }
static void pin_in_pullup(uint pin) { gpio_init(pin); gpio_set_dir(pin, GPIO_IN); gpio_pull_up(pin); }

int main(void) {
  vreg_set_voltage(VREG_VOLTAGE_1_15);               // tegangan core naik dulu agar stabil di atas 133 MHz
  sleep_ms(2);
  set_sys_clock_khz(SYS_KHZ, true);
  cfg_load();
  stdio_init_all();
  pin_out(PIN_SA, 0); pin_out(PIN_DA, 0); pin_out(PIN_SB, 0); pin_out(PIN_DB, 0); pin_out(PIN_EN, 0);
  pin_in_pullup(PIN_LX); pin_in_pullup(PIN_LY);
  servo_init();
  multicore_launch_core1(core1_main);

  char buf[80]; int len = 0;
  for (;;) {
    int ch;
    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
      if (ch == '\n' || ch == '\r') { buf[len] = 0; handle(buf); len = 0; }
      else if (len < (int)sizeof(buf) - 1) buf[len++] = (char)ch;
    }
    pump();
  }
}

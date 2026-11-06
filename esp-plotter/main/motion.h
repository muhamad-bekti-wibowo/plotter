// Mesin gerak CoreXY: task FreeRTOS di core 1, antrean item gerak, servo LEDC.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { OP_MOVE = 0, OP_ZERO, OP_PEN_DN, OP_PEN_UP, OP_HOME_DONE, OP_MARK, OP_CAL_FINISH } mot_op_t;

// Bit saklar batas: 1 = X min (kiri), 2 = Y min (bawah), 4 = X max (kanan), 8 = Y max (atas)

#define FL_NO_ACCEL 1    // tanpa akselerasi awal (sambung dari segmen sebelumnya)
#define FL_NO_DECEL 2    // tanpa perlambatan akhir
#define FL_NOGUARD  4    // abaikan pengawal saklar (gerak mundur dari saklar)
#define FL_REARM    8    // gerak cari-saklar: saklar yang sudah tertekan di awal diabaikan sampai dilepas, baru tekanan berikutnya berhenti
                         // (kiri+kanan disambung ke satu pin: tekan lagi = saklar seberang)

typedef struct {
  uint8_t op;
  uint8_t stop;          // mask saklar (lihat atas): berhenti begitu salah satunya tertekan; 0 = gerak biasa. OP_MARK: fl = nomor penanda 0..3
  uint8_t fl;
  int32_t a, b;          // langkah motor A, B (relatif)
  float v;               // kecepatan alat mm/s, 0 = vmax
} mot_item_t;

#define MOT_QLEN 400

void motion_init(void);
const char* motion_apply_pins(void);         // NULL jika sukses, selain itu pesan kesalahan (hanya saat diam)
bool motion_push(const mot_item_t* it);      // tidak memblokir, false jika antrean penuh
int motion_room(void);                       // slot kosong
bool motion_idle(void);                      // tidak ada item tertunda
int motion_pending(void);
void motion_abort(void);                     // hentikan segera & kosongkan antrean
void motion_pos(int32_t* a, int32_t* b);
void motion_set_pos(int32_t a, int32_t b);   // hanya saat diam: tetapkan posisi sekarang (langkah)
bool motion_pen(void);
uint32_t motion_limits(void);                // 4 bit: X min, Y min, X max, Y max
bool motion_has_max(void);
int motion_raw_level(int sw);                // diagnosis: level GPIO mentah saklar 0..3 (X min, Y min, X max, Y max); -1 = tidak dipasang
int motion_sw_pin(int sw);                   // nomor GPIO saklar, -1 = tidak dipasang                   // saklar kanan & atas terpasang (pinLX2/pinLY2 >= 0)
void motion_enable(bool on);
void motion_servo_angle(float deg);          // langsung (hanya saat diam)

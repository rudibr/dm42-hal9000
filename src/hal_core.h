// HAL 9000 for DM42 - platform independent core
#pragma once
#include <stdint.h>

typedef struct {
  int year, month, day;   // month 1..12
  int hour, min, sec;
} hal_time_t;

// Called for every output character (already with the name substituted).
// Return nonzero to abort generation.
typedef int (*hal_emit_fn)(char c, void *ctx);

int  hal_init(uint32_t seed);              // 0 = ok, -1 = out of memory
void hal_set_name(const char *name);
const char *hal_get_name(void);

// Opening line (asks for the user's name)
void hal_intro(const hal_time_t *t, hal_emit_fn emit, void *ctx);

// Is HAL still waiting for the user's name?
int  hal_waiting_for_name(void);

// Respond to one line of user input
void hal_respond(const char *input, const hal_time_t *t, hal_emit_fn emit, void *ctx);

// Day of week 0=Monday..6=Sunday
int  hal_dow(int y, int m, int d);

// Special action requested by the last response (UI may play sound)
#define HAL_ACT_SING 1
int  hal_last_action(void);

// Daisy Bell melody (public domain, 1892): pairs of {Hz, duration in 1/8 beats}, 0-terminated
extern const uint16_t hal_daisy[][2];

// Live "ship" readings supplied by the platform
typedef struct {
  int mv;          // battery / supply voltage in millivolts (0 = unknown)
  int usb;         // 1 = running on USB power
  int lowbat;      // 1 = low battery warning
  int free_bytes;  // free heap memory
} hal_sys_t;
void hal_set_sysinfo(void (*fn)(hal_sys_t *));

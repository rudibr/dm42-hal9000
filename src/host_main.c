// Host (PC) build of HAL for testing:  ./hal_host
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "hal_core.h"

static int emit(char c, void *ctx) { (void)ctx; putchar(c); fflush(stdout); return 0; }

static void now(hal_time_t *t) {
  time_t s = time(0); struct tm *m = localtime(&s);
  t->year = m->tm_year + 1900; t->month = m->tm_mon + 1; t->day = m->tm_mday;
  t->hour = m->tm_hour; t->min = m->tm_min; t->sec = m->tm_sec;
  const char *fake = getenv("HAL_HOUR");      // for testing: HAL_HOUR=21
  if (fake) t->hour = atoi(fake);
}

static void fake_sys(hal_sys_t *s) {   // pretend values on the PC
  s->mv = 2940; s->usb = getenv("HAL_USB") != 0; s->lowbat = 0; s->free_bytes = 41 * 1024;
}

int main(void) {
  hal_set_sysinfo(fake_sys);
  hal_time_t t; now(&t);
  if (hal_init((uint32_t)time(0))) { puts("out of memory"); return 1; }
  printf("HAL: "); hal_intro(&t, emit, 0); printf("\n");
  char line[128];
  while (printf("> "), fflush(stdout), fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    now(&t);
    printf("HAL: "); hal_respond(line, &t, emit, 0);
    if (hal_last_action() == HAL_ACT_SING) printf("  [plays Daisy Bell]");
    printf("\n");
  }
  return 0;
}

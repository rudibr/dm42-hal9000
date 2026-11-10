// HAL 9000 for SwissMicros DM42 (DMCP program)
//
// Keys (ABC mode, default)   letters as printed on the DM42 keys
//   [+] space   [.] .   [0] ,   [R/S] ?   [-] -
//   [ENTER] send   [<-] backspace   [EXIT] clear line   [UP]/[DOWN] scroll
//   [SHIFT]+key  one character from the other mode (digits / operators)
//   [SHIFT][ENTER] lock 123 mode on/off      [SHIFT][0] setup menu
//   [SHIFT][EXIT] power off                  [SHIFT][F6] sound on/off
//   [F1]..[F6]   quick questions
#include <string.h>
#include <stdlib.h>

#include <main.h>
#include <dmcp.h>
#include <menu.h>

#include "hal_core.h"

// ------------------------------------------------------------------ keymaps
//                       index = DMCP key code (1..37)
static const char ABC[38] = { 0,
  'A','B','C','D','E','F',      //  1-6   S+  1/x  sqrt LOG LN  XEQ
  'G','H','I','J','K','L',      //  7-12  STO RCL  Rdn  SIN COS TAN
   0, 'M','N','O', 0,           // 13-17  ENTER x<>y +/- E  <-
   0, 'P','Q','R','S',          // 18-22  UP  7  8  9  /
   0, 'T','U','V','W',          // 23-27  DN  4  5  6  *
   0, 'X','Y','Z','-',          // 28-32  SHIFT 1 2 3 -
   0, ',','.','?',' ' };        // 33-37  EXIT 0 . R/S +
static const char NUM[38] = { 0,
  '(',')','^', 0,  0,  0,
   0,  0,  0,  0,  0,  0,
   0, ' ', 0,  0,  0,
   0, '7','8','9','/',
   0, '4','5','6','*',
   0, '1','2','3','-',
   0, '0','.','?','+' };

static const char *FKEY_LABEL[6] = {"Who?", "Status", "Doors", "Time", "Sing", "Help"};
static const char *FKEY_TEXT[6]  = {"who are you", "status report",
  "open the pod bay doors", "what time is it", "sing for me", "help"};

// ------------------------------------------------------------------ chat log
#define HIST   48
#define LLEN   64
static char (*lines)[LLEN];
static int  nlines, scroll;
static char input[72];
static int  inlen;
static int  shift, numlock, sound = 1, thinking, eye_phase;

static int chat_top(void)    { return lcd_lineHeight(t20) + 2; }
static int input_y(void)     { return LCD_Y - LCD_MENU_LINES - lcd_lineHeight(t20) - 1; }
static int chat_rows(void)   { return (input_y() - 2 - chat_top()) / lcd_lineHeight(t20); }

static void new_line(void) {
  if (nlines == HIST) { memmove(lines[0], lines[1], (HIST - 1) * LLEN); nlines--; }
  lines[nlines++][0] = 0;
}

static void append_char(char c) {
  if (!nlines) new_line();
  char *l = lines[nlines - 1]; int n = strlen(l);
  if (n >= LLEN - 1) { new_line(); l = lines[nlines - 1]; n = 0; }
  if (n == 0 && c == ' ') return;
  l[n] = c; l[n + 1] = 0;
  if (lcd_textWidth(t20, l) > LCD_X - 8) {
    char *sp = strrchr(l, ' ');
    char tail[LLEN];
    if (sp && sp != l) { strcpy(tail, sp + 1); *sp = 0; }
    else { tail[0] = c; tail[1] = 0; l[n] = 0; }
    new_line(); strcpy(lines[nlines - 1], tail);
  }
}
static void append_str(const char *s) { while (*s) append_char(*s++); }

// ------------------------------------------------------------------ drawing
static void disc(int cx, int cy, int r, int val) {
  for (int dy = -r; dy <= r; dy++) {
    int w = 0; while ((w + 1) * (w + 1) + dy * dy <= r * r) w++;
    lcd_fill_rect(cx - w, cy + dy, 2 * w + 1, 1, val);
  }
}

static void annun(int x, const char *txt) {
  t20->lnfill = 0; t20->x = x; t20->y -= 2;
  lcd_fill_rect(t20->x, 1, lcd_textWidth(t20, txt), lcd_lineHeight(t20) - 5, 0);
  lcd_puts(t20, txt);
  t20->y += 2; t20->lnfill = 1;
}

static void draw_header(void) {
  tm_t tm; dt_t dt; char s[24];
  rtc_read(&tm, &dt);
  lcd_writeClr(t20);
  t20->newln = 0;
  lcd_putsR(t20, " HAL 9000");
  annun(150, numlock ? "[123]" : "[ABC]");
  if (shift) annun(210, "[SHIFT]");
  if (!sound) annun(290, "[MUTE]");
  s[0] = '0' + tm.hour / 10; s[1] = '0' + tm.hour % 10; s[2] = ':';
  s[3] = '0' + tm.min / 10;  s[4] = '0' + tm.min % 10;  s[5] = 0;
  annun(LCD_X - lcd_textWidth(t20, s) - 4, s);
  t20->newln = 1;
  // The eye
  int cy = lcd_lineHeight(t20) / 2, cx = 120;
  disc(cx, cy, 9, 0);
  disc(cx, cy, 7, 1);
  int pr = thinking ? 2 + (eye_phase & 3) : 3;
  disc(cx, cy, pr, 0);
}

static void draw(void) {
  lcd_clear_buf();
  draw_header();
  int lh = lcd_lineHeight(t20), rows = chat_rows();
  int first = nlines - rows - scroll; if (first < 0) first = 0;
  t20->newln = 0;
  for (int i = 0; i < rows && first + i < nlines; i++) {
    lcd_setXY(t20, 4, chat_top() + i * lh);
    lcd_puts(t20, lines[first + i]);
  }
  // input line
  int iy = input_y();
  lcd_fill_rect(0, iy - 2, LCD_X, 1, 1);
  char buf[80]; buf[0] = '>'; buf[1] = ' ';
  const char *p = input;
  while (lcd_textWidth(t20, p) > LCD_X - 40 && *p) p++;   // show the tail
  strcpy(buf + 2, p); strcat(buf, thinking ? "" : "_");
  lcd_setXY(t20, 4, iy); lcd_puts(t20, buf);
  t20->newln = 1;
  lcd_draw_menu_keys(FKEY_LABEL);
  lcd_refresh();
}

// ------------------------------------------------------------------ HAL glue
static void dm42_sys(hal_sys_t *s) {
  int mv = get_vbat();
  if (mv < 1000 || mv > 6000) mv = (int)read_power_voltage();
  if (mv < 1000 || mv > 6000) mv = 0;
  s->mv = mv;
  s->usb = usb_powered() ? 1 : 0;
  s->lowbat = get_lowbat_state() ? 1 : 0;
  s->free_bytes = sys_free_mem();
}

static void now(hal_time_t *t) {
  tm_t tm; dt_t dt; rtc_read(&tm, &dt);
  t->year = dt.year; t->month = dt.month; t->day = dt.day;
  t->hour = tm.hour; t->min = tm.min; t->sec = tm.sec;
}

static void click(void) {
  if (!sound || ST(STAT_BEEP_MUTE)) return;
  start_buzzer_freq(1800000 + (rand() % 6) * 300000);
  sys_delay(2); stop_buzzer();
}

static int emit(char c, void *ctx) {
  (void)ctx;
  append_char(c);
  eye_phase++;
  if (c != ' ' && (eye_phase & 1)) click();
  reset_auto_off();
  draw();
  if (!key_empty()) { int k = key_pop(); if (k == KEY_EXIT) return 1; }
  return 0;
}

static void play_daisy(void) {
  if (!sound || ST(STAT_BEEP_MUTE)) return;
  const int eighth = 140;
  for (int i = 0; hal_daisy[i][1]; i++) {
    int ms = hal_daisy[i][1] * eighth;
    if (hal_daisy[i][0]) start_buzzer_freq(hal_daisy[i][0] * 1000);
    sys_delay(ms * 9 / 10); stop_buzzer(); sys_delay(ms / 10);
    eye_phase++; draw();
    if (!key_empty() && key_pop() == KEY_EXIT) break;
  }
  stop_buzzer();
}

static void ask(const char *text) {
  hal_time_t t; now(&t);
  scroll = 0;
  new_line(); append_str("> "); append_str(text);
  new_line();
  thinking = 1; draw();
  hal_respond(text, &t, emit, 0);
  if (hal_last_action() == HAL_ACT_SING) play_daisy();
  thinking = 0;
}

// ------------------------------------------------------------------ keys
static void handle_key(int key) {
  if (key <= 0) return;
  if (shift) {
    shift = 0;
    switch (key) {
      case KEY_0:
        SET_ST(STAT_MENU); handle_menu(&MID_MENU, MENU_RESET, 0); CLR_ST(STAT_MENU);
        wait_for_key_release(-1); return;
      case KEY_EXIT:  SET_ST(STAT_PGM_END); return;
      case KEY_ENTER: numlock = !numlock; return;
      case KEY_F6:    sound = !sound; return;
      case KEY_SHIFT: return;
    }
    if (key <= 37) {
      char c = numlock ? ABC[key] : NUM[key];
      if (c && inlen < (int)sizeof(input) - 1) { input[inlen++] = c; input[inlen] = 0; }
    }
    return;
  }
  switch (key) {
    case KEY_SHIFT: shift = 1; return;
    case KEY_EXIT:  inlen = 0; input[0] = 0; return;
    case KEY_BSP:   if (inlen) input[--inlen] = 0; return;
    case KEY_UP:    if (scroll < nlines - chat_rows()) scroll++; return;
    case KEY_DOWN:  if (scroll) scroll--; return;
    case KEY_ENTER:
      if (inlen || !hal_waiting_for_name()) {
        char q[72]; strcpy(q, input); inlen = 0; input[0] = 0; ask(q);
      }
      return;
  }
  if (key >= KEY_F1 && key <= KEY_F6) {
    if (hal_waiting_for_name()) return;   // name first, please
    ask(FKEY_TEXT[key - KEY_F1]); return;
  }
  if (key <= 37) {
    char c = numlock ? NUM[key] : ABC[key];
    if (c && inlen < (int)sizeof(input) - 1) { input[inlen++] = c; input[inlen] = 0; }
  }
}

// ------------------------------------------------------------------ main
void program_main(void) {
  run_menu_item_app = run_menu_item;
  menu_line_str_app = menu_line_str;

  hal_set_sysinfo(dm42_sys);
  lines = malloc(HIST * LLEN);
  srand(get_rtc_ticks() ^ sys_tick_count());
  if (!lines || hal_init(get_rtc_ticks() * 2654435761u + sys_tick_count())) {
    lcd_clear_buf(); lcd_putsAt(t24, 3, "HAL: out of memory"); lcd_refresh();
    wait_for_key_press(); SET_ST(STAT_PGM_END);
  } else {
    hal_time_t t; now(&t);
    thinking = 1; draw();
    hal_intro(&t, emit, 0);
    thinking = 0;
  }
  SET_ST(STAT_CLK_WKUP_ENABLE);   // wake every minute to update the clock
  draw();

  for (;;) {
    if ((ST(STAT_PGM_END) && ST(STAT_SUSPENDED)) || (!ST(STAT_PGM_END) && key_empty())) {
      CLR_ST(STAT_RUNNING);
      sys_sleep();
    }
    if (ST(STAT_PGM_END) || ST(STAT_SUSPENDED)) {
      if (!ST(STAT_SUSPENDED)) {
        lcd_set_buf_cleared(0);
        draw_power_off_image(1);
        LCD_power_off(0);
        SET_ST(STAT_SUSPENDED);
        SET_ST(STAT_OFF);
      }
      continue;
    }
    SET_ST(STAT_RUNNING);
    CLR_ST(STAT_SUSPENDED);
    if (ST(STAT_OFF)) {
      LCD_power_on();
      rtc_wakeup_delay();
      CLR_ST(STAT_OFF);
      if (!lcd_get_buf_cleared()) lcd_forced_refresh();
    }
    CLR_ST(STAT_CLK_WKUP_FLAG);
    if (!key_empty()) reset_auto_off();
    int key = key_pop();
    handle_key(key);
    draw();
  }
}

// newlib's abort() (pulled in by libc) needs _exit; never expected to run
void _exit(int status) { (void)status; sys_reset(); for (;;) ; }

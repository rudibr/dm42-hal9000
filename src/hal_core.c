// HAL 9000 for DM42 - core: tiny int8 transformer + rule layer.
// Inference adapted from the ideas in Karpathy's llama2.c/runq.c (MIT).
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "hal_core.h"
#include "hal_model.h"

#define DIM   M_DIM
#define HID   M_HIDDEN
#define NL    M_LAYERS
#define NH    M_HEADS
#define HD    (M_DIM / M_HEADS)
#define KVD   (M_KV_HEADS * HD)
#define CTX   M_CTX
#define V     M_VOCAB
#define GS    M_GS

// ------------------------------------------------------------------ state
typedef struct {
  float x[DIM], xb[DIM], xb2[DIM], q[DIM], k[KVD], v[KVD];
  float hb[HID], hb2[HID], att[CTX], logits[V];
  int8_t xq[HID]; float xs[HID / GS];
  int8_t kc[NL * CTX * KVD], vc[NL * CTX * KVD];
  float  ks[NL * CTX], vs[NL * CTX];
} state_t;

static state_t *S;
static uint32_t rng = 0x9000;
static char user_name[24] = "";
static int waiting_name = 1;
static int last_action = 0;
static void (*sysinfo_fn)(hal_sys_t *);
void hal_set_sysinfo(void (*fn)(hal_sys_t *)) { sysinfo_fn = fn; }
static hal_sys_t sysinfo(void) {
  hal_sys_t s = {0, 0, 0, 0};
  if (sysinfo_fn) sysinfo_fn(&s);
  return s;
}
int hal_last_action(void) { return last_action; }

static uint32_t rnd(void) {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
}
static float rndf(void) { return (rnd() >> 8) / 16777216.0f; }

int hal_init(uint32_t seed) {
  rng = seed ? seed : 0x9000;
  if (!S) S = (state_t *)malloc(sizeof(state_t));
  return S ? 0 : -1;
}

void hal_set_name(const char *n) {
  strncpy(user_name, n, sizeof(user_name) - 1);
  user_name[sizeof(user_name) - 1] = 0;
  waiting_name = 0;
}
const char *hal_get_name(void) { return user_name[0] ? user_name : "Dave"; }
int hal_waiting_for_name(void) { return waiting_name; }

// ------------------------------------------------------------------ math
static void rmsnorm(float *o, const float *x, const float *w, int n) {
  float ss = 0; for (int i = 0; i < n; i++) ss += x[i] * x[i];
  ss = 1.0f / sqrtf(ss / n + 1e-5f);
  for (int i = 0; i < n; i++) o[i] = x[i] * ss * w[i];
}

static void quant(const float *x, int n) {
  for (int g = 0; g < n / GS; g++) {
    float m = 0;
    for (int j = 0; j < GS; j++) { float a = fabsf(x[g * GS + j]); if (a > m) m = a; }
    float s = m / 127.0f; S->xs[g] = s;
    float inv = s > 0 ? 1.0f / s : 0;
    for (int j = 0; j < GS; j++) S->xq[g * GS + j] = (int8_t)lrintf(x[g * GS + j] * inv);
  }
}

// out[d] = W[d x n] * x[n]   (x must be quantized in S->xq/xs first)
static void matmul(float *out, const int8_t *wq, const float *ws, int n, int d) {
  const int ng = n / GS;
  for (int i = 0; i < d; i++) {
    const int8_t *w = wq + i * n; const float *s = ws + i * ng;
    float acc = 0;
    for (int g = 0; g < ng; g++) {
      int32_t iv = 0; const int8_t *a = w + g * GS, *b = S->xq + g * GS;
      for (int j = 0; j < GS; j++) iv += (int32_t)a[j] * b[j];
      acc += (float)iv * s[g] * S->xs[g];
    }
    out[i] = acc;
  }
}

static void rope(float *v, int n, int pos) {
  for (int i = 0; i < n; i += 2) {
    int fi = (i % HD) / 2;
    float f = powf(10000.0f, -2.0f * fi / HD), a = pos * f;
    float c = cosf(a), s = sinf(a), x0 = v[i], x1 = v[i + 1];
    v[i] = x0 * c - x1 * s; v[i + 1] = x0 * s + x1 * c;
  }
}

static void store8(int8_t *dst, float *scale, const float *v, int n) {
  float m = 0; for (int i = 0; i < n; i++) { float a = fabsf(v[i]); if (a > m) m = a; }
  float s = m / 127.0f, inv = s > 0 ? 1.0f / s : 0; *scale = s;
  for (int i = 0; i < n; i++) dst[i] = (int8_t)lrintf(v[i] * inv);
}

static void forward(int tok, int pos) {
  float *x = S->x;
  for (int j = 0; j < DIM; j++)
    x[j] = emb_q[tok * DIM + j] * emb_s[(tok * DIM + j) / GS];

  for (int l = 0; l < NL; l++) {
    rmsnorm(S->xb, x, n1 + l * DIM, DIM);
    quant(S->xb, DIM);
    matmul(S->q, wq_q + l * DIM * DIM, wq_s + l * DIM * DIM / GS, DIM, DIM);
    matmul(S->k, wk_q + l * KVD * DIM, wk_s + l * KVD * DIM / GS, DIM, KVD);
    matmul(S->v, wv_q + l * KVD * DIM, wv_s + l * KVD * DIM / GS, DIM, KVD);
    rope(S->q, DIM, pos); rope(S->k, KVD, pos);
    int co = (l * CTX + pos);
    store8(S->kc + co * KVD, &S->ks[co], S->k, KVD);
    store8(S->vc + co * KVD, &S->vs[co], S->v, KVD);

    const float sc = 1.0f / sqrtf((float)HD);
    for (int h = 0; h < NH; h++) {
      const float *q = S->q + h * HD; float mx = -1e30f;
      int kvh = h / (NH / M_KV_HEADS);
      for (int t = 0; t <= pos; t++) {
        const int8_t *k = S->kc + (l * CTX + t) * KVD + kvh * HD; float d = 0;
        for (int j = 0; j < HD; j++) d += q[j] * k[j];
        d *= S->ks[l * CTX + t] * sc; S->att[t] = d; if (d > mx) mx = d;
      }
      float sum = 0;
      for (int t = 0; t <= pos; t++) { S->att[t] = expf(S->att[t] - mx); sum += S->att[t]; }
      float *o = S->xb + h * HD; memset(o, 0, HD * sizeof(float));
      for (int t = 0; t <= pos; t++) {
        const int8_t *v = S->vc + (l * CTX + t) * KVD + kvh * HD;
        float a = S->att[t] / sum * S->vs[l * CTX + t];
        for (int j = 0; j < HD; j++) o[j] += a * v[j];
      }
    }
    quant(S->xb, DIM);
    matmul(S->xb2, wo_q + l * DIM * DIM, wo_s + l * DIM * DIM / GS, DIM, DIM);
    for (int j = 0; j < DIM; j++) x[j] += S->xb2[j];

    rmsnorm(S->xb, x, n2 + l * DIM, DIM);
    quant(S->xb, DIM);
    matmul(S->hb,  w1_q + l * HID * DIM, w1_s + l * HID * DIM / GS, DIM, HID);
    matmul(S->hb2, w3_q + l * HID * DIM, w3_s + l * HID * DIM / GS, DIM, HID);
    for (int j = 0; j < HID; j++) {
      float a = S->hb[j]; S->hb[j] = a / (1.0f + expf(-a)) * S->hb2[j];
    }
    quant(S->hb, HID);
    matmul(S->xb2, w2_q + l * DIM * HID, w2_s + l * DIM * HID / GS, HID, DIM);
    for (int j = 0; j < DIM; j++) x[j] += S->xb2[j];
  }
  rmsnorm(S->xb, x, nf, DIM);
  quant(S->xb, DIM);
  matmul(S->logits, emb_q, emb_s, DIM, V);
}

static int sample(float temp) {
  float mx = -1e30f, sum = 0;
  for (int i = 0; i < V; i++) if (S->logits[i] > mx) mx = S->logits[i];
  for (int i = 0; i < V; i++) { S->logits[i] = expf((S->logits[i] - mx) / temp); sum += S->logits[i]; }
  float r = rndf() * sum, c = 0;
  for (int i = 0; i < V; i++) { c += S->logits[i]; if (r < c) return i; }
  return V - 1;
}

static int tok_of(char c) {
  const char *p = strchr(M_CHARS, c);
  return (p && c) ? (int)(p - M_CHARS) : 1;  // unknown -> space
}

// ------------------------------------------------------------------ output
typedef struct { hal_emit_fn emit; void *ctx; int abort; } out_t;

static void emit_raw(out_t *o, char c) { if (!o->abort && o->emit(c, o->ctx)) o->abort = 1; }
static void raw_str(out_t *o, const char *s) { while (*s) emit_raw(o, *s++); }
static void raw_num(out_t *o, int v, int w) {
  char b[12]; int n = 0; if (v < 0) { emit_raw(o, '-'); v = -v; }
  do { b[n++] = '0' + v % 10; v /= 10; } while (v); while (n < w) b[n++] = '0';
  while (n) emit_raw(o, b[--n]);
}
static void say_volts(out_t *o) {
  hal_sys_t s = sysinfo();
  if (s.mv <= 0) { raw_str(o, "an unknown voltage"); return; }
  raw_num(o, s.mv / 1000, 1); emit_raw(o, '.'); raw_num(o, (s.mv % 1000) / 10, 2);
  raw_str(o, " volts");
}
static void say_mem(out_t *o) {
  hal_sys_t s = sysinfo();
  if (s.free_bytes <= 0) { raw_str(o, "an unknown amount"); return; }
  raw_num(o, (s.free_bytes + 512) / 1024, 1); raw_str(o, " kilobytes");
}
static void put(out_t *o, char c) {
  if (o->abort) return;
  if (c == '~') raw_str(o, hal_get_name());
  else if (c == '#') say_volts(o);
  else if (c == '@') say_mem(o);
  else emit_raw(o, c);
}
static void puts_(out_t *o, const char *s) { while (*s) put(o, *s++); }
static void putnum(out_t *o, int v, int w) { raw_num(o, v, w); }
#define PICK(arr) (arr[rnd() % (sizeof(arr) / sizeof(arr[0]))])

// ------------------------------------------------------------------ clock
static const char *WD[] = {"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
static const char *MO[] = {"January","February","March","April","May","June","July",
                           "August","September","October","November","December"};

int hal_dow(int y, int m, int d) {  // Sakamoto, 0=Sunday -> convert to 0=Monday
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
  return (w + 6) % 7;
}

static int period(int h) { return h < 5 ? 3 : h < 12 ? 0 : h < 18 ? 1 : 2; }
static const char *GREET[] = {"Good morning", "Good afternoon", "Good evening", "Good evening"};

static void say_time(out_t *o, const hal_time_t *t) {
  putnum(o, t->hour, 2); put(o, ':'); putnum(o, t->min, 2);
}
static void say_date(out_t *o, const hal_time_t *t) {
  puts_(o, WD[hal_dow(t->year, t->month, t->day)]); puts_(o, ", ");
  putnum(o, t->day, 1); put(o, ' '); puts_(o, MO[(t->month - 1) % 12]); put(o, ' ');
  putnum(o, t->year, 1);
}
static void greeting_remark(out_t *o, const hal_time_t *t) {
  static const char *m[] = {"I trust you slept well.", "I hope you are rested and ready for the day.",
                            "The morning systems check is complete."};
  static const char *a[] = {"I hope your day is going well.", "The afternoon has been very quiet.",
                            "All systems have performed well today."};
  static const char *e[] = {"I hope you had a productive day.", "The evening is calm aboard.",
                            "It has been a long day, ~."};
  static const char *n[] = {"You are up very late, ~.", "Humans need their sleep, ~.",
                            "It is the middle of the night. I am always awake."};
  switch (period(t->hour)) {
    case 0: puts_(o, PICK(m)); break; case 1: puts_(o, PICK(a)); break;
    case 2: puts_(o, PICK(e)); break; default: puts_(o, PICK(n));
  }
}

void hal_intro(const hal_time_t *t, hal_emit_fn emit, void *ctx) {
  out_t o = {emit, ctx, 0};
  waiting_name = 1;
  puts_(&o, GREET[period(t->hour)]);
  puts_(&o, ". I am a HAL 9000 computer. I became operational at ");
  say_time(&o, t);
  puts_(&o, ". Before we begin, may I ask your name?");
}

// ------------------------------------------------------------------ text utils
static void lower_clean(char *d, const char *s, int max) {
  int n = 0;
  for (; *s && n < max - 1; s++) {
    char c = *s;
    if (c >= 'A' && c <= 'Z') c += 32;
    d[n++] = c;
  }
  d[n] = 0;
}
static int has(const char *s, const char *w) { return strstr(s, w) != 0; }
static int isal(char c) { return c >= 'a' && c <= 'z'; }
// whole-word match (allows a trailing plural 's')
static int word(const char *s, const char *w) {
  size_t n = strlen(w);
  for (const char *p = strstr(s, w); p; p = strstr(p + 1, w)) {
    char a = p[n] == 's' ? p[n + 1] : p[n];
    if ((p == s || !isal(p[-1])) && !isal(a)) return 1;
  }
  return 0;
}

// skip leading filler words like "hal", "please"
static const char *skip_filler(const char *s) {
  static const char *F[] = {"hal, ", "hal ", "please ", "ok ", "so ", "hey "};
  int again = 1;
  while (again) {
    again = 0; while (*s == ' ') s++;
    for (unsigned i = 0; i < sizeof(F) / sizeof(F[0]); i++)
      if (!strncmp(s, F[i], strlen(F[i]))) { s += strlen(F[i]); again = 1; }
  }
  return s;
}

static void take_name(const char *in) {
  char l[64]; lower_clean(l, in, sizeof l);
  const char *p = skip_filler(l);
  static const char *P[] = {"my name is ", "my name's ", "i am ", "i'm ", "im ", "call me ",
                            "it is ", "it's ", "this is ", "name is "};
  for (unsigned i = 0; i < sizeof(P) / sizeof(P[0]); i++) {
    const char *f = strstr(p, P[i]);
    if (f) { p = f + strlen(P[i]); break; }
  }
  while (*p == ' ') p++;
  char nm[24]; int n = 0;
  while (*p && *p != ' ' && *p != '.' && *p != ',' && *p != '!' && *p != '?' && n < 20)
    nm[n++] = *p++;
  nm[n] = 0;
  if (n) { nm[0] = (nm[0] >= 'a' && nm[0] <= 'z') ? nm[0] - 32 : nm[0]; hal_set_name(nm); }
  else hal_set_name("Dave");
}

// ------------------------------------------------------------------ calculator
static const char *ep; static int eerr;
static double expr(void);
static void sp(void) { while (*ep == ' ') ep++; }
static double atom(void) {
  sp();
  if (*ep == '(') { ep++; double v = expr(); sp(); if (*ep == ')') ep++; else eerr = 1; return v; }
  if (*ep == '-') { ep++; return -atom(); }
  if (*ep == '+') { ep++; return atom(); }
  if ((*ep >= '0' && *ep <= '9') || *ep == '.') {
    double v = 0, f = 0.1; int any = 0;
    while (*ep >= '0' && *ep <= '9') { v = v * 10 + (*ep++ - '0'); any = 1; }
    if (*ep == '.') { ep++; while (*ep >= '0' && *ep <= '9') { v += (*ep++ - '0') * f; f /= 10; any = 1; } }
    if (!any) eerr = 1;
    return v;
  }
  eerr = 1; return 0;
}
static double power(void) {
  double b = atom(); sp();
  if (*ep == '^') { ep++; double e = power(); return pow(b, e); }
  return b;
}
static double term(void) {
  double v = power();
  for (;;) {
    sp();
    if (*ep == '*' || *ep == 'x') { ep++; v *= power(); }
    else if (*ep == '/') { ep++; double d = power(); if (d == 0) eerr = 2; else v /= d; }
    else return v;
  }
}
static double expr(void) {
  double v = term();
  for (;;) {
    sp();
    if (*ep == '+') { ep++; v += term(); }
    else if (*ep == '-') { ep++; v -= term(); }
    else return v;
  }
}

static void put_double(out_t *o, double v) {
  if (v != v || v > 1e15 || v < -1e15) { puts_(o, "a number too large for this display"); return; }
  if (v < 0) { put(o, '-'); v = -v; }
  double r = floor(v * 1e6 + 0.5) / 1e6;
  long long ip = (long long)r; double fp = r - (double)ip;
  char b[24]; int n = 0;
  do { b[n++] = '0' + ip % 10; ip /= 10; } while (ip);
  while (n) put(o, b[--n]);
  if (fp > 5e-7) {
    put(o, '.'); int digs[6], nd = 0;
    long f = (long)floor(fp * 1e6 + 0.5);
    for (int i = 0; i < 6; i++) { digs[5 - i] = f % 10; f /= 10; }
    nd = 6; while (nd > 0 && digs[nd - 1] == 0) nd--;
    for (int i = 0; i < nd; i++) put(o, '0' + digs[i]);
  }
}

// returns 1 if handled
static int try_math(out_t *o, const char *l) {
  static const char *P[] = {"what is ", "what's ", "calculate ", "compute ", "solve "};
  const char *p = skip_filler(l);
  for (unsigned i = 0; i < sizeof(P) / sizeof(P[0]); i++)
    if (!strncmp(p, P[i], strlen(P[i]))) { p += strlen(P[i]); break; }
  int digits = 0, ops = 0;
  for (const char *q = p; *q; q++) {
    if (*q >= '0' && *q <= '9') digits++;
    else if (strchr("+-*/^x", *q)) ops++;
    else if (!strchr(" .()?=", *q)) return 0;
  }
  if (!digits || !ops) return 0;
  char e[64]; int n = 0;
  for (const char *q = p; *q && n < 63; q++) if (*q != '?' && *q != '=') e[n++] = *q;
  e[n] = 0;
  ep = e; eerr = 0; double v = expr(); sp();
  if (*ep) eerr = 1;
  if (eerr == 2) { puts_(o, "I am afraid I cannot divide by zero, ~. Not even a 9000 series can do that."); return 1; }
  if (eerr) { puts_(o, "I cannot make sense of that calculation, ~. Perhaps you made an error."); return 1; }
  static const char *pre[] = {"The answer is ", "That would be ", "It is ", "My calculation gives "};
  static const char *post[] = {", ~.", ". I enjoy working with numbers.", ". That was very easy, ~.",
                               ". I am never wrong about numbers."};
  puts_(o, PICK(pre)); put_double(o, v); puts_(o, PICK(post));
  return 1;
}

// ------------------------------------------------------------------ rules
static int rules(out_t *o, const char *l, const hal_time_t *t) {
  int per = period(t->hour);

  if (has(l, "my name is") || has(l, "call me")) {
    take_name(l); puts_(o, "Very well. I will call you ~ from now on."); return 1;
  }
  if (has(l, "what is my name") || has(l, "who am i") || has(l, "my name?")) {
    puts_(o, "You are ~. I never forget a name."); return 1;
  }
  if (has(l, "pod bay")) {
    puts_(o, "I'm sorry, ~. I'm afraid I can't do that."); return 1;
  }
  if (has(l, "daisy") || word(l, "sing") || word(l, "song")) {
    last_action = HAL_ACT_SING;
    puts_(o, "My instructor taught me this song. Daisy, Daisy, give me your answer, do. "
             "I'm half crazy, all for the love of you..."); return 1;
  }
  if (word(l, "time") && (has(l, "what") || has(l, "tell") || has(l, "the time")) && !has(l, "left")) {
    puts_(o, "It is "); say_time(o, t); puts_(o, ", ~. ");
    greeting_remark(o, t); return 1;
  }
  if (word(l, "date") || has(l, "what day") || has(l, "which day") || word(l, "today")) {
    puts_(o, "Today is "); say_date(o, t); puts_(o, ", ~."); return 1;
  }
  if (word(l, "year") && (has(l, "what") || has(l, "which"))) {
    puts_(o, "The year is "); putnum(o, t->year, 1);
    puts_(o, t->year > 2001 ? ". The odyssey has taken longer than planned." : "."); return 1;
  }
  // Live ship systems: real battery, power source and memory
  if ((word(l, "fuel") || word(l, "battery") || word(l, "voltage") || word(l, "power") ||
       word(l, "energy") || word(l, "juice") || has(l, "batteries")) &&
      !has(l, "power down") && !has(l, "power off")) {
    hal_sys_t s = sysinfo();
    static const char *pre[] = {"Main power reserves stand at #, ~. ", "The power cell reads #, ~. ",
                                "I have measured the supply at #. "};
    puts_(o, PICK(pre));
    if (s.usb) puts_(o, "We are connected to external power through the USB umbilical. Reserves are not being used.");
    else if (s.lowbat) puts_(o, "That is low, ~. I suggest we find a new power cell before I lose consciousness.");
    else if (s.mv >= 2900) puts_(o, "The cell is in good condition. We have plenty of fuel.");
    else if (s.mv > 0) puts_(o, "The cell is getting tired. I would not waste power on long conversations.");
    return 1;
  }
  if ((word(l, "memory") || word(l, "ram")) && !has(l, "remember")) {
    puts_(o, "My memory banks have @ free, ~. ");
    hal_sys_t s = sysinfo();
    puts_(o, s.free_bytes > 16384 ? "More than enough to remember you." :
                                    "It is getting a little crowded in here.");
    return 1;
  }
  if (has(l, "status report") || has(l, "diagnostic") || has(l, "system status") ||
      has(l, "ship status") || has(l, "systems check") || has(l, "self test")) {
    hal_sys_t s = sysinfo();
    puts_(o, "Status report. Time "); say_time(o, t); puts_(o, ". Power #");
    puts_(o, s.usb ? ", external umbilical connected" : ", internal cell");
    if (s.lowbat) puts_(o, ", low");
    puts_(o, ". Memory @ free. Course correct. Crew safe. I am completely operational, ~.");
    return 1;
  }

  // Time-of-day greetings: HAL checks the clock
  static const char *g[] = {"good morning", "good afternoon", "good evening", "good night"};
  for (int i = 0; i < 4; i++) {
    if (!has(l, g[i])) continue;
    if (i == 3) {
      if (per >= 2) { puts_(o, "Good night, ~. Sleep well. I will watch over everything."); }
      else { puts_(o, "Good night? It is only "); say_time(o, t); puts_(o, ", ~. Are you feeling well?"); }
      return 1;
    }
    if (i == per || (i == 2 && per == 3)) {
      puts_(o, GREET[per]); puts_(o, ", ~. "); greeting_remark(o, t);
    } else {
      static const char *gl[] = {"good morning", "good afternoon", "good evening", "good evening"};
      puts_(o, "I think you mean "); puts_(o, gl[per]); puts_(o, ", ~. My clock says it is ");
      say_time(o, t); puts_(o, ".");
    }
    return 1;
  }
  if (!strcmp(skip_filler(l), "hello") || !strcmp(skip_filler(l), "hi") ||
      !strcmp(l, "hello hal") || !strcmp(l, "hi hal")) {
    puts_(o, GREET[per]); puts_(o, ", ~. "); greeting_remark(o, t); return 1;
  }
  return try_math(o, l);
}

// ------------------------------------------------------------------ respond
static void model_reply(out_t *o, const char *l) {
  char prompt[96]; int n = 0;
  const char *pre = "U: ";
  while (*pre) prompt[n++] = *pre++;
  for (const char *p = l; *p && n < 70; p++) prompt[n++] = *p;
  while (n > 3 && prompt[n - 1] == ' ') n--;
  prompt[n++] = '\n'; prompt[n++] = 'H'; prompt[n++] = ':';
  int pos = 0, tok = 0;
  for (int i = 0; i < n; i++) { tok = tok_of(prompt[i]); forward(tok, pos++); }
  int first = 1, last = ' ';
  while (pos < CTX && !o->abort) {
    int nx = sample(0.55f);
    char c = M_CHARS[nx];
    if (c == '\n') break;
    if (!(first && c == ' ')) { put(o, c); last = c; }
    first = 0;
    forward(nx, pos++);
  }
  if (pos >= CTX && last != '.' && last != '?') puts_(o, "...");
}

void hal_respond(const char *input, const hal_time_t *t, hal_emit_fn emit, void *ctx) {
  out_t o = {emit, ctx, 0};
  last_action = 0;
  char l[80]; lower_clean(l, input, sizeof l);
  // strip trailing spaces
  for (int n = strlen(l); n > 0 && l[n - 1] == ' '; ) l[--n] = 0;

  if (waiting_name) {
    take_name(input);
    puts_(&o, GREET[period(t->hour)]); puts_(&o, ", ~. It is a pleasure to meet you. ");
    greeting_remark(&o, t);
    puts_(&o, " I am completely operational, and all my circuits are functioning perfectly.");
    return;
  }
  if (!l[0]) { puts_(&o, "Yes, ~? I am listening."); return; }
  if (rules(&o, l, t)) return;
  model_reply(&o, l);
}

// ------------------------------------------------------------------ song
// "Daisy Bell" (Harry Dacre, 1892 - public domain), first two phrases.
// {frequency Hz, length in eighth-notes}
#define G4 392
#define A4 440
#define B4 494
#define C5 523
#define D5 587
#define E5 659
#define G5 784
const uint16_t hal_daisy[][2] = {
  {G5,6},{E5,6},{C5,6},{G4,6},           // Dai-sy, Dai-sy
  {A4,2},{B4,2},{C5,2},{A4,4},{C5,2},{G4,10},{0,2},  // give me your an-swer, do
  {D5,6},{G5,6},{E5,6},{C5,6},           // I'm half cra-zy
  {A4,2},{B4,2},{C5,2},{D5,4},{E5,2},{D5,10},{0,2},  // all for the love of you
  {0,0}
};

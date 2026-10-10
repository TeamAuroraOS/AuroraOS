/* The CPU stress test; see stress.h and ../README.md. The same source runs on
 * both systems. The workloads (work.c) run on the CPU under test through
 * plat_work_start() and plat_work_wait(); this file sizes them, times them,
 * samples the console once a second, draws both screens and writes the
 * report. */
#include "stress.h"
#include "draw.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES_MAX 720
#define MAX_WORKERS 3

/* How long one job of work runs between screen updates, how often the
 * screens update while the test idles, and how long a clock check takes. */
#define WORK_SLICE_MS 250u
#define IDLE_SLICE_MS 100u
#define CLOCK_CHECK_MS 5u

#define C_BG     0x101418u
#define C_PANEL  0x1A2028u
#define C_GRID   0x2A323Cu
#define C_TEXT   0xE6E8EBu
#define C_DIM    0x8A939Eu
#define C_ACCENT 0x64E8C8u
#define C_LOAD   0x2F7A6Au
#define C_SPEED  0xF2F2F2u
#define C_TEMP   0xFF9F0Au
#define C_BAD    0xFF5A4Au

enum { PH_IDLE, PH_CLOCK, PH_INT, PH_FLOAT, PH_MEM, PH_STRESS, PH_COOL, PH_COUNT };
#define ST_READY (-1)
#define ST_DONE  PH_COUNT

static const char *const ph_name[PH_COUNT] = {
    "Idle", "Clock", "Integer", "Float", "Memory", "Stress", "Cooldown"};
static const char *const ph_about[PH_COUNT] = {
    "nothing running: a baseline", "measuring the clock",
    "integer arithmetic",          "floating point",
    "memory bandwidth",            "all workloads, flat out",
    "nothing running again"};
static const uint32_t ph_color[PH_COUNT] = {0x5A6470, 0x3B82F6, 0xA05CE2,
                                            0xFF2DB8, 0xFFD60A, 0xFF5A4A,
                                            0x5A6470};
static uint32_t ph_secs[PH_COUNT] = {5, 3, 5, 5, 6, 60, 10};

static const uint32_t lengths[] = {30, 60, 120, 300, 600};
#define LENGTHS (int)(sizeof(lengths) / sizeof(lengths[0]))

/* What one repetition of each workload counts as: adds, iterations,
 * operations, bytes, rounds. */
static const double per_rep_units[W_COUNT] = {
    CLOCK_PASSES * 256.0, INT_CHUNK, FLOAT_CHUNK * 8.0, MEM_BYTES,
    MEM_BYTES,            MEM_BYTES, 1.0};

typedef struct {
  uint16_t t10;   /* tenths of a second since the start */
  uint8_t phase;
  uint8_t load;   /* % of the time the core under test ran workloads */
  float rate;     /* stress rounds per second of work, the core under test */
  float worked;   /* seconds of stress work in this sample */
  float extra;    /* stress rounds per second, the other cores */
  float mhz;
  int16_t temp;
  int16_t tenths;
  int16_t mv;
  int8_t charging;
} Sample;

static struct {
  const PlatInfo *pi;
  int state, stopped, failed, length;
  uint64_t hz, t_start, t_phase, t_sample, t_end;
  uint64_t busy_acc, work_acc; /* this sample: all jobs, stress jobs */
  uint32_t rounds_acc, rounds_total;
  uint64_t stress_ticks;
  double units[W_COUNT];
  uint64_t ticks[W_COUNT];
  uint64_t per_rep[W_COUNT]; /* ticks one repetition took last time */
  Sample s[SAMPLES_MAX];
  int ns;
  StressWorker w[MAX_WORKERS];
  int workers, running; /* started this run, still running */
  uint32_t w_start[MAX_WORKERS], w_seen[MAX_WORKERS], w_total[MAX_WORKERS];
  WorkCtx *ctx;
  Battery bat;
  char date[32], notes[160];
  char saved[128];
  int save_ok;
} g;

static uint64_t now(void) { return plat_ticks(); }

static double secs(uint64_t ticks) { return (double)ticks / (double)g.hz; }

static uint32_t total_secs(void) {
  uint32_t t = 0;
  for (int i = 0; i < PH_COUNT; i++)
    t += ph_secs[i];
  return t;
}

static uint32_t phase_start_secs(int ph) {
  uint32_t t = 0;
  for (int i = 0; i < ph; i++)
    t += ph_secs[i];
  return t;
}

static double rate(int kind) {
  return g.ticks[kind] ? g.units[kind] / secs(g.ticks[kind]) : 0.0;
}

static double mhz_result(void) { return rate(W_CLOCK) / 1e6; }

/* Repetitions that take about `ms`, from the last job of the kind. */
static uint32_t reps_for(uint32_t kind, uint32_t ms) {
  uint64_t want = (uint64_t)ms * g.hz / 1000u, r;
  if (!g.per_rep[kind])
    return 1;
  r = want / g.per_rep[kind];
  return r < 1u ? 1u : r > 1000000u ? 1000000u : (uint32_t)r;
}

static void job_start(uint32_t kind, uint32_t reps) {
  plat_work_start(kind, reps);
}

/* The ticks the job took, also counted as busy; 0 when it failed. */
static uint64_t job_wait(uint32_t kind, uint32_t reps) {
  uint64_t t = plat_work_wait();
  if (!t) {
    g.failed = 1;
    return 0;
  }
  g.per_rep[kind] = t / reps ? t / reps : 1u;
  g.busy_acc += t;
  return t;
}

static void count(uint32_t kind, uint32_t reps, uint64_t t) {
  g.units[kind] += per_rep_units[kind] * reps;
  g.ticks[kind] += t;
}

static float measure_mhz(void) {
  uint32_t reps = reps_for(W_CLOCK, CLOCK_CHECK_MS);
  uint64_t t;
  job_start(W_CLOCK, reps);
  t = job_wait(W_CLOCK, reps);
  return t ? (float)(per_rep_units[W_CLOCK] * reps / secs(t) / 1e6) : 0.0f;
}

void stress_worker(StressWorker *w) {
  WorkCtx c;
  memset(&c, 0, sizeof(c));
  c.seed = 0x1234567u + (uint32_t)w->core;
  c.ra = malloc(ROUND_MEM);
  c.rb = malloc(ROUND_MEM);
  if (c.ra && c.rb) {
    memset(c.ra, 0x5A, ROUND_MEM);
    while (!w->stop) {
      work_run(&c, W_ROUND, 1);
      w->rounds = w->rounds + 1u;
    }
  }
  free(c.ra);
  free(c.rb);
}

static void present(void) { plat_present(); }

static void sample_battery(Sample *s) {
  plat_battery(&g.bat);
  s->temp = (int16_t)(g.bat.temp_c < -100 ? TEMP_UNKNOWN : g.bat.temp_c);
  s->tenths = (int16_t)g.bat.tenths;
  s->mv = (int16_t)g.bat.mv;
  s->charging = (int8_t)g.bat.charging;
}

/* The starting temperature, charge and clock, before anything runs. */
static void first_sample(void) {
  Sample *s = &g.s[g.ns++];
  memset(s, 0, sizeof(*s));
  s->phase = PH_IDLE;
  s->mhz = measure_mhz();
  sample_battery(s);
  g.busy_acc = 0;
}

static void take_sample(uint64_t t) {
  uint64_t window = t - g.t_sample;
  Sample *s;
  uint32_t extra = 0;
  if (!window || g.ns >= SAMPLES_MAX)
    return;
  s = &g.s[g.ns++];
  memset(s, 0, sizeof(*s));
  s->t10 = (uint16_t)(secs(t - g.t_start) * 10.0 + 0.5);
  s->phase = (uint8_t)(g.state < PH_COUNT ? g.state : PH_COOL);
  s->load = (uint8_t)(g.busy_acc >= window ? 100 : g.busy_acc * 100 / window);
  s->rate = g.work_acc ? (float)(g.rounds_acc / secs(g.work_acc)) : 0.0f;
  s->worked = (float)secs(g.work_acc);
  for (int i = 0; i < g.workers; i++) {
    uint32_t r = g.w[i].rounds;
    extra += r - g.w_seen[i];
    g.w_seen[i] = r;
  }
  s->extra = (float)(extra / secs(window));
  g.busy_acc = g.work_acc = 0;
  g.rounds_acc = 0;
  g.t_sample = t;
  s->mhz = g.failed ? 0.0f : measure_mhz(); /* counted in the next second */
  sample_battery(s);
}

static const Sample *last_sample(void) { return g.ns ? &g.s[g.ns - 1] : 0; }

/* A second shared with the phase before is mostly other work. */
static int stress_second(const Sample *s) {
  return s->phase == PH_STRESS && s->worked >= 0.5f;
}

static void cores_start(void) {
  memset(g.w, 0, sizeof(g.w));
  g.workers = g.running = plat_cores_start(g.w, MAX_WORKERS);
  for (int i = 0; i < g.workers; i++)
    g.w_start[i] = g.w_seen[i] = g.w[i].rounds;
}

static void cores_stop(void) {
  if (!g.running)
    return;
  for (int i = 0; i < g.workers; i++)
    g.w_total[i] = g.w[i].rounds - g.w_start[i];
  plat_cores_stop(g.w, g.running);
  g.running = 0;
}

static void report_save(void);

static void enter(int ph) {
  if (ph == ST_DONE)
    take_sample(now());
  if (g.state == PH_STRESS) {
    g.stress_ticks = now() - g.t_phase;
    cores_stop();
  }
  g.state = ph;
  g.t_phase = now();
  if (ph == PH_STRESS)
    cores_start();
  if (ph == ST_DONE) {
    g.t_end = now();
    report_save();
  }
}

static int buffers(void) {
  WorkCtx *c = g.ctx;
  if (!c->big_a)
    c->big_a = malloc(MEM_BYTES);
  if (!c->big_b)
    c->big_b = malloc(MEM_BYTES);
  if (!c->ra)
    c->ra = malloc(ROUND_MEM);
  if (!c->rb)
    c->rb = malloc(ROUND_MEM);
  if (!c->big_a || !c->big_b || !c->ra || !c->rb)
    return 0;
  memset(c->big_a, 0x5A, MEM_BYTES);
  memset(c->ra, 0x3C, ROUND_MEM);
  return 1;
}

static void start(void) {
  if (!buffers()) {
    snprintf(g.saved, sizeof(g.saved), "not enough memory for the test");
    g.save_ok = 0;
    return;
  }
  g.ctx->seed = 0xC0FFEEu;
  ph_secs[PH_STRESS] = lengths[g.length];
  memset(g.units, 0, sizeof(g.units));
  memset(g.ticks, 0, sizeof(g.ticks));
  memset(g.w_total, 0, sizeof(g.w_total));
  g.ns = 0;
  g.stopped = g.failed = 0;
  g.workers = g.running = 0;
  g.rounds_total = 0;
  g.stress_ticks = 0;
  g.saved[0] = 0;
  plat_date(g.date, sizeof(g.date));
  plat_notes(g.notes, sizeof(g.notes));
  g.t_start = now();
  g.busy_acc = g.work_acc = 0;
  g.rounds_acc = 0;
  first_sample();
  g.state = ST_READY;
  enter(PH_IDLE);
  g.t_sample = now();
}

static void draw_screens(void);

/* One slice of the current phase: a job sized to about WORK_SLICE_MS, with
 * the screens drawn while it runs, or a short idle. */
static void run_slice(void) {
  uint64_t end = g.t_phase + (uint64_t)ph_secs[g.state] * g.hz, left;
  uint32_t kind, reps, ms;
  uint64_t t;
  if (g.state == PH_IDLE || g.state == PH_COOL) {
    draw_screens();
    present();
    plat_wait(IDLE_SLICE_MS);
    return;
  }
  switch (g.state) {
    case PH_CLOCK: kind = W_CLOCK; break;
    case PH_INT: kind = W_INT; break;
    case PH_FLOAT: kind = W_FLOAT; break;
    case PH_MEM: {
      double in = secs(now() - g.t_phase);
      kind = in < ph_secs[PH_MEM] / 3.0         ? W_READ
             : in < ph_secs[PH_MEM] * 2.0 / 3.0 ? W_WRITE
                                                 : W_COPY;
      break;
    }
    default: kind = W_ROUND; break;
  }
  left = now() < end ? end - now() : 0;
  ms = WORK_SLICE_MS;
  if (left * 1000u / g.hz < ms)
    ms = (uint32_t)(left * 1000u / g.hz) + 1u;
  reps = reps_for(kind, ms);
  job_start(kind, reps);
  draw_screens();
  t = job_wait(kind, reps);
  if (t) {
    count(kind, reps, t);
    if (kind == W_ROUND) {
      g.rounds_acc += reps;
      g.rounds_total += reps;
      g.work_acc += t;
    }
  }
  present();
}

static void step(void) {
  uint64_t t;
  run_slice();
  if (g.failed) {
    g.stopped = 1;
    enter(ST_DONE);
    return;
  }
  t = now();
  if (t - g.t_sample >= g.hz)
    take_sample(t);
  if (t - g.t_phase >= (uint64_t)ph_secs[g.state] * g.hz)
    enter(g.state + 1);
}

typedef struct {
  double rate_avg, rate_min, rate_max, extra_avg, load_avg, mhz_min, mhz_max;
  int n;
  int temp_start, temp_max, temp_end, tenths_start, tenths_end, mv_start,
      mv_end;
} Summary;

static void summarize(Summary *m) {
  memset(m, 0, sizeof(*m));
  m->rate_min = 1e30;
  m->mhz_min = 1e30;
  m->temp_start = m->temp_max = m->temp_end = TEMP_UNKNOWN;
  m->tenths_start = m->tenths_end = m->mv_start = m->mv_end = -1;
  for (int i = 0; i < g.ns; i++) {
    const Sample *s = &g.s[i];
    if (s->temp != TEMP_UNKNOWN) {
      if (m->temp_start == TEMP_UNKNOWN)
        m->temp_start = s->temp;
      if (m->temp_max == TEMP_UNKNOWN || s->temp > m->temp_max)
        m->temp_max = s->temp;
      m->temp_end = s->temp;
    }
    if (s->tenths >= 0) {
      if (m->tenths_start < 0)
        m->tenths_start = s->tenths;
      m->tenths_end = s->tenths;
    }
    if (s->mv >= 0) {
      if (m->mv_start < 0)
        m->mv_start = s->mv;
      m->mv_end = s->mv;
    }
    if (!stress_second(s))
      continue;
    m->n++;
    m->rate_avg += s->rate;
    m->extra_avg += s->extra;
    m->load_avg += s->load;
    if (s->rate < m->rate_min)
      m->rate_min = s->rate;
    if (s->rate > m->rate_max)
      m->rate_max = s->rate;
    if (s->mhz < m->mhz_min)
      m->mhz_min = s->mhz;
    if (s->mhz > m->mhz_max)
      m->mhz_max = s->mhz;
  }
  if (m->n) {
    m->rate_avg /= m->n;
    m->extra_avg /= m->n;
    m->load_avg /= m->n;
  } else {
    m->rate_min = m->mhz_min = 0.0;
  }
}

static char *g_rep;
static size_t g_rep_len, g_rep_cap;

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) {
  va_list ap;
  int n;
  if (g_rep_len >= g_rep_cap)
    return;
  va_start(ap, fmt);
  n = vsnprintf(g_rep + g_rep_len, g_rep_cap - g_rep_len, fmt, ap);
  va_end(ap);
  if (n > 0)
    g_rep_len += (size_t)n < g_rep_cap - g_rep_len ? (size_t)n
                                                   : g_rep_cap - g_rep_len - 1;
}

static void temp_text(char *o, int n, int t) {
  if (t == TEMP_UNKNOWN)
    snprintf(o, n, "unknown");
  else
    snprintf(o, n, "%d C", t);
}

static double per_mhz(double v) {
  double mhz = mhz_result();
  return mhz > 0 ? v / mhz : 0.0;
}

static void report_build(void) {
  Summary m;
  char a[24], b[24], c[24];
  double run = secs(g.t_end - g.t_start);
  double overall = g.stress_ticks ? g.rounds_total / secs(g.stress_ticks) : 0.0;
  summarize(&m);
  out("CPU stress test results\r\n=======================\r\n\r\n");
  out("System:        %s\r\n", g.pi->system);
  out("Console:       %s\r\n", g.pi->model);
  out("CPU tested:    %s\r\n", g.pi->cpu);
  out("Settings:      %s\r\n", g.notes);
  out("Build:         %s\r\n", g.pi->build);
  out("Date:          %s (console clock)\r\n", g.date);
  out("Stress length: %u s%s\r\n", (unsigned)ph_secs[PH_STRESS],
      g.failed    ? "  (stopped: the CPU under test did not answer)"
      : g.stopped ? "  (stopped early)"
                  : "");
  out("Run time:      %.1f s\r\n\r\n", run);

  out("Speed (one core)                                    per MHz\r\n");
  out("  Clock:          %8.1f MHz (256 dependent adds a pass)\r\n",
      mhz_result());
  out("  Integer:        %8.2f M iterations/s           %8.4f\r\n",
      rate(W_INT) / 1e6, per_mhz(rate(W_INT) / 1e6));
  out("  Float:          %8.2f MFLOPS (single)          %8.4f\r\n",
      rate(W_FLOAT) / 1e6, per_mhz(rate(W_FLOAT) / 1e6));
  out("  Memory read:    %8.1f MB/s (4 MB buffer)       %8.4f\r\n",
      rate(W_READ) / 1e6, per_mhz(rate(W_READ) / 1e6));
  out("  Memory write:   %8.1f MB/s                     %8.4f\r\n",
      rate(W_WRITE) / 1e6, per_mhz(rate(W_WRITE) / 1e6));
  out("  Memory copy:    %8.1f MB/s                     %8.4f\r\n\r\n",
      rate(W_COPY) / 1e6, per_mhz(rate(W_COPY) / 1e6));

  out("Stress (integer + float + 64 KB copy per round)\r\n");
  out("  This core:      %.1f rounds/s while working, %.1f overall\r\n",
      m.rate_avg, overall);
  out("  Steadiness:     slowest second %.1f, fastest %.1f rounds/s (%.1f%% "
      "spread)\r\n",
      m.rate_min, m.rate_max,
      m.rate_avg > 0 ? (m.rate_max - m.rate_min) * 100.0 / m.rate_avg : 0.0);
  out("  Load:           %.1f %% of the time running the workloads\r\n",
      m.load_avg);
  out("  Clock under load: %.1f - %.1f MHz\r\n", m.mhz_min, m.mhz_max);
  if (g.workers) {
    double total = overall;
    out("  Other cores:    %d\r\n", g.workers);
    for (int i = 0; i < g.workers; i++) {
      double r = g.stress_ticks ? g.w_total[i] / secs(g.stress_ticks) : 0.0;
      total += r;
      out("    core %d:       %.1f rounds/s\r\n", g.w[i].core, r);
    }
    out("  All cores:      %.1f rounds/s\r\n\r\n", total);
  } else {
    out("  Other cores:    none\r\n");
    out("  All cores:      %.1f rounds/s\r\n\r\n", overall);
  }

  temp_text(a, sizeof(a), m.temp_start);
  temp_text(b, sizeof(b), m.temp_max);
  temp_text(c, sizeof(c), m.temp_end);
  out("Temperature (the battery's, MCU register 0x0A: the 3DS has no CPU "
      "sensor software can read)\r\n");
  out("  Start: %s, highest: %s, end: %s", a, b, c);
  if (m.temp_start != TEMP_UNKNOWN && m.temp_max != TEMP_UNKNOWN)
    out(", rise: %+d C", m.temp_max - m.temp_start);
  out("\r\n\r\n");

  out("Battery\r\n");
  if (m.tenths_start >= 0)
    out("  Charge:   %d.%d %% -> %d.%d %%\r\n", m.tenths_start / 10,
        m.tenths_start % 10, m.tenths_end / 10, m.tenths_end % 10);
  else
    out("  Charge:   unknown\r\n");
  if (m.mv_start >= 0)
    out("  Voltage:  %d.%02d V -> %d.%02d V (system, load side)\r\n",
        m.mv_start / 1000, m.mv_start % 1000 / 10, m.mv_end / 1000,
        m.mv_end % 1000 / 10);
  else
    out("  Voltage:  unknown\r\n");
  out("  Charging: %s\r\n\r\n",
      g.bat.charging < 0 ? "unknown" : g.bat.charging ? "yes" : "no");

  out("Samples (about one a second; rounds/s only while stressing)\r\n");
  out("    time  phase      load  rounds/s  other cores     MHz  temp  "
      "charge  volts\r\n");
  for (int i = 0; i < g.ns; i++) {
    const Sample *s = &g.s[i];
    char t[8] = "  -";
    if (s->temp != TEMP_UNKNOWN)
      snprintf(t, sizeof(t), "%3d", s->temp);
    out("  %6.1f  %-9s  %3u%%  %8.1f  %11.1f  %6.1f  %4s  %4d.%d  %d.%02d\r\n",
        s->t10 / 10.0, ph_name[s->phase], (unsigned)s->load, s->rate,
        s->extra, s->mhz, t, s->tenths < 0 ? 0 : s->tenths / 10,
        s->tenths < 0 ? 0 : s->tenths % 10, s->mv < 0 ? 0 : s->mv / 1000,
        s->mv < 0 ? 0 : s->mv % 1000 / 10);
  }
}

static void report_save(void) {
  FILE *f;
  g_rep_cap = 96u * 1024u;
  g_rep = malloc(g_rep_cap);
  g_rep_len = 0;
  if (!g_rep) {
    snprintf(g.saved, sizeof(g.saved), "Not saved: out of memory");
    g.save_ok = 0;
    return;
  }
  g_rep[0] = 0;
  report_build();
  f = fopen(g.pi->result_path, "wb");
  g.save_ok = f && fwrite(g_rep, 1, g_rep_len, f) == g_rep_len;
  if (f && fclose(f) != 0)
    g.save_ok = 0;
  snprintf(g.saved, sizeof(g.saved),
           g.save_ok ? "Saved to %s" : "Could not save %s", g.pi->result_name);
  free(g_rep);
  g_rep = 0;
}

#define GX0 30
#define GY0 32
#define GW  334
#define GH  168

static int gx(double t) {
  uint32_t total = total_secs();
  return GX0 + (int)(t * GW / (total ? total : 1));
}

static int gy(double pct) {
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  return GY0 + GH - 1 - (int)(pct * (GH - 1) / 100.0);
}

/* The temperature axis: 30 degrees from a multiple of 5 under the start. */
static int temp_lo(void) {
  for (int i = 0; i < g.ns; i++)
    if (g.s[i].temp != TEMP_UNKNOWN) {
      int lo = (g.s[i].temp - 8) / 5 * 5;
      return lo < 0 ? 0 : lo;
    }
  return 20;
}

static void draw_top(void) {
  Fb f = {plat_fb(0), 400};
  int lo = temp_lo(), x;
  double best = 0;
  d_clear(&f, C_BG);
  d_textf(&f, 4, 4, C_ACCENT, "CPU stress test");
  d_textf(&f, 4 + 16 * 8, 4, C_TEXT, "%s  %.20s", g.pi->title,
          g.pi->cpu_short);

  x = 6;
  d_rect(&f, x, 18, 10, 8, C_LOAD);
  x += 14 + d_text(&f, x + 14, 18, C_DIM, "Load %");
  x += 12;
  d_rect(&f, x, 21, 10, 2, C_SPEED);
  x += 14 + d_text(&f, x + 14, 18, C_DIM, "Speed % (stress)");
  x += 12;
  d_rect(&f, x, 21, 10, 2, C_TEMP);
  d_text(&f, x + 14, 18, C_DIM, "Battery C");

  for (int p = 0; p <= 100; p += 25) {
    d_rect(&f, GX0, gy(p), GW, 1, C_GRID);
    if (p % 50 == 0)
      d_textf(&f, GX0 - 4 - (p == 100 ? 24 : p ? 16 : 8), gy(p) - 3, C_DIM,
              "%d", p);
  }
  for (int k = 0; k <= 30; k += 15)
    d_textf(&f, GX0 + GW + 4, gy(k * 100.0 / 30.0) - 3, C_TEMP, "%dC", lo + k);
  d_rect(&f, GX0, GY0, 1, GH, C_GRID);
  d_rect(&f, GX0 + GW - 1, GY0, 1, GH, C_GRID);

  /* Phases under the graph. */
  for (int p = 0; p < PH_COUNT; p++) {
    int x0 = gx(phase_start_secs(p)), x1 = gx(phase_start_secs(p + 1));
    d_rect(&f, x0, GY0 + GH + 3, x1 - x0 - 1, 5, ph_color[p]);
    if ((int)strlen(ph_name[p]) * 8 <= x1 - x0)
      d_text(&f, x0, GY0 + GH + 11, ph_color[p], ph_name[p]);
  }
  d_textf(&f, GX0 + GW - 8 * 6, GY0 + GH + 23, C_DIM, "%5us",
          (unsigned)total_secs());
  d_text(&f, GX0, GY0 + GH + 23, C_DIM, "0s");

  for (int i = 0; i < g.ns; i++)
    if (stress_second(&g.s[i]) && g.s[i].rate > best)
      best = g.s[i].rate;
  for (int i = 1; i < g.ns; i++) {
    const Sample *a = &g.s[i - 1], *b = &g.s[i];
    int x0 = gx(a->t10 / 10.0), x1 = gx(b->t10 / 10.0);
    d_rect(&f, x0 + 1, gy(b->load), x1 - x0, GY0 + GH - gy(b->load), C_LOAD);
    if (stress_second(a) && stress_second(b) && best > 0)
      d_line(&f, x0, gy(a->rate * 100.0 / best), x1, gy(b->rate * 100.0 / best),
             C_SPEED);
    if (a->temp != TEMP_UNKNOWN && b->temp != TEMP_UNKNOWN)
      d_line(&f, x0, gy((a->temp - lo) * 100.0 / 30.0), x1,
             gy((b->temp - lo) * 100.0 / 30.0), C_TEMP);
  }
  if (g.state >= 0 && g.state < ST_DONE)
    d_rect(&f, gx(secs(now() - g.t_start)), GY0, 1, GH, C_DIM);
  if (g.state == ST_READY)
    d_text(&f, GX0 + GW / 2 - 8 * 8, GY0 + GH / 2 - 4, C_TEXT,
           "Press A to start");
}

static void bar(Fb *f, int x, int y, int w, double part) {
  if (part < 0)
    part = 0;
  if (part > 1)
    part = 1;
  d_rect(f, x, y, w, 8, C_GRID);
  d_rect(f, x, y, (int)(w * part), 8, C_ACCENT);
}

static void line_value(Fb *f, int y, const char *label, uint32_t c,
                       const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void line_value(Fb *f, int y, const char *label, uint32_t c,
                       const char *fmt, ...) {
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  d_text(f, 8, y, C_DIM, label);
  d_text(f, 8 + 11 * 8, y, c, buf);
}

static void draw_bottom(void) {
  Fb f = {plat_fb(1), 320};
  const Sample *s = last_sample();
  Summary m;
  int y = 6;
  char t1[16];
  d_clear(&f, C_PANEL);
  d_textf(&f, 8, y, C_ACCENT, "%s", g.pi->title);
  d_textf(&f, 8 + 10 * 8, y, C_DIM, "%.29s", g.pi->model);
  y += 12;
  d_textf(&f, 8, y, C_TEXT, "%.38s", g.pi->cpu_short);
  y += 10;
  if (g.state == ST_READY)
    plat_notes(g.notes, sizeof(g.notes));
  d_textf(&f, 8, y, C_DIM, "%.38s", g.notes);
  y += 14;
  d_rect(&f, 0, y - 6, 320, 1, C_GRID);

  if (g.state == ST_READY) {
    d_text(&f, 8, y, C_TEXT, "Ready.");
    y += 12;
    d_textf(&f, 8, y, C_DIM, "Idle, clock, integer, float, memory,");
    y += 10;
    d_textf(&f, 8, y, C_DIM, "then %u s of stress, then cooldown.",
            (unsigned)lengths[g.length]);
    y += 10;
    d_textf(&f, 8, y, C_DIM, "Results go to %.24s", g.pi->result_name);
    y += 10;
    if (g.saved[0])
      d_textf(&f, 8, y, C_BAD, "%.38s", g.saved);
    else if (plat_option())
      d_textf(&f, 8, y, C_TEXT, "%.38s", plat_option());
  } else if (g.state < ST_DONE) {
    double in = secs(now() - g.t_phase), all = secs(now() - g.t_start);
    d_textf(&f, 8, y, ph_color[g.state], "%s", ph_name[g.state]);
    d_textf(&f, 8 + 10 * 8, y, C_DIM, "%.28s", ph_about[g.state]);
    y += 12;
    bar(&f, 8, y, 200, in / ph_secs[g.state]);
    d_textf(&f, 216, y, C_TEXT, "%3u/%us", (unsigned)in,
            (unsigned)ph_secs[g.state]);
    y += 12;
    d_textf(&f, 8, y, C_DIM, "Total %u:%02u of %u:%02u", (unsigned)all / 60,
            (unsigned)all % 60, (unsigned)total_secs() / 60,
            (unsigned)total_secs() % 60);
  } else {
    d_textf(&f, 8, y, g.stopped ? C_TEMP : C_ACCENT, "%s",
            g.failed    ? "Stopped: no answer from the CPU."
            : g.stopped ? "Stopped early."
                        : "Done.");
    y += 12;
    d_textf(&f, 8, y, g.save_ok ? C_TEXT : C_BAD, "%.38s", g.saved);
    y += 12;
    d_textf(&f, 8, y, C_DIM, "Run time %.0f s", secs(g.t_end - g.t_start));
  }
  y = 106;
  d_rect(&f, 0, y - 6, 320, 1, C_GRID);

  summarize(&m);
  if (g.ticks[W_CLOCK])
    line_value(&f, y, "Clock", C_TEXT, "%.1f MHz", mhz_result());
  else
    line_value(&f, y, "Clock", C_DIM, "-");
  y += 10;
  if (g.ticks[W_INT])
    line_value(&f, y, "Integer", C_TEXT, "%.2f M it/s", rate(W_INT) / 1e6);
  else
    line_value(&f, y, "Integer", C_DIM, "-");
  y += 10;
  if (g.ticks[W_FLOAT])
    line_value(&f, y, "Float", C_TEXT, "%.2f MFLOPS", rate(W_FLOAT) / 1e6);
  else
    line_value(&f, y, "Float", C_DIM, "-");
  y += 10;
  if (g.ticks[W_READ])
    line_value(&f, y, "Memory", C_TEXT, "R %.0f W %.0f C %.0f MB/s",
               rate(W_READ) / 1e6, rate(W_WRITE) / 1e6, rate(W_COPY) / 1e6);
  else
    line_value(&f, y, "Memory", C_DIM, "-");
  y += 10;
  if (m.n)
    line_value(&f, y, "Stress", C_TEXT, "%.1f rounds/s", m.rate_avg);
  else
    line_value(&f, y, "Stress", C_DIM, "-");
  y += 10;
  if (g.running && s)
    line_value(&f, y, "Cores", C_TEXT, "+%d others: %.1f rounds/s",
               g.workers, s->extra);
  else if (g.workers)
    line_value(&f, y, "Cores", C_TEXT, "+%d others: %.1f rounds/s",
               g.workers, m.extra_avg);
  else if (g.state == ST_READY)
    line_value(&f, y, "Cores", C_DIM, "-");
  else
    line_value(&f, y, "Cores", C_DIM, "this one only");
  y += 14;
  d_rect(&f, 0, y - 6, 320, 1, C_GRID);
  if (s) {
    line_value(&f, y, "Now", C_TEXT, "load %u%%  %.1f MHz", (unsigned)s->load,
               s->mhz);
  }
  y += 10;
  plat_battery(&g.bat);
  temp_text(t1, sizeof(t1), g.bat.temp_c);
  if (g.bat.tenths >= 0)
    line_value(&f, y, "Battery", C_TEXT, "%s  %d.%d%%  %d.%02dV%s", t1,
               g.bat.tenths / 10, g.bat.tenths % 10, g.bat.mv / 1000,
               g.bat.mv % 1000 / 10, g.bat.charging == 1 ? "  +" : "");
  else
    line_value(&f, y, "Battery", C_TEXT, "%s", t1);
  y += 10;
  if (m.temp_start != TEMP_UNKNOWN)
    line_value(&f, y, "Temp", C_TEXT, "start %d C, highest %d C",
               m.temp_start, m.temp_max);

  d_rect(&f, 0, 226, 320, 14, C_BG);
  if (g.state == ST_READY)
    d_textf(&f, 4, 229, C_DIM, "A: start  L/R: stress %us  START: quit",
            (unsigned)lengths[g.length]);
  else if (g.state < ST_DONE)
    d_text(&f, 4, 229, C_DIM, "START or B: stop early (results saved)");
  else
    d_text(&f, 4, 229, C_DIM, "A: run again   START: quit");
}

static void draw_screens(void) {
  draw_top();
  draw_bottom();
}

int stress_main(const PlatInfo *pi) {
  memset(&g, 0, sizeof(g));
  g.pi = pi;
  g.hz = plat_tick_hz();
  g.state = ST_READY;
  g.length = 1;
  g.ctx = plat_work_ctx();
  for (;;) {
    uint32_t k = 0;
    if (!plat_update(&k))
      break;
    if (g.state == ST_READY || g.state == ST_DONE) {
      if (k & K_START)
        break;
      if (g.state == ST_READY) {
        if ((k & (K_L | K_LEFT)) && g.length > 0)
          g.length--;
        if ((k & (K_R | K_RIGHT)) && g.length < LENGTHS - 1)
          g.length++;
        if ((k & K_X) && plat_option())
          plat_option_toggle();
        if (k & K_A)
          start();
      } else if (k & K_A) {
        g.state = ST_READY;
        g.saved[0] = 0;
      }
      draw_screens();
      present();
      continue;
    }
    if (k & (K_START | K_B)) {
      g.stopped = 1;
      enter(ST_DONE);
      draw_screens();
      present();
      continue;
    }
    step();
  }
  cores_stop();
  free(g.ctx->big_a);
  free(g.ctx->big_b);
  free(g.ctx->ra);
  free(g.ctx->rb);
  g.ctx->big_a = g.ctx->big_b = g.ctx->ra = g.ctx->rb = 0;
  return 0;
}

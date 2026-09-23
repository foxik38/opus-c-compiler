// report.c - the pipeline status display (written to stderr).
//
// On a terminal the display is live: while a stage runs, a spinner and a
// running clock are drawn on its line by a helper thread, and the line is
// replaced by the stage's result when it finishes. The summary ends with a
// bar showing where the build time went. When stderr is not a terminal (or
// with --no-color / NO_COLOR) the output is plain text, one line per stage.
#include "driver/report.h"

#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_DIM "\033[2m"
#define C_GREEN "\033[1;32m"
#define C_YELLOW "\033[1;33m"
#define C_RED "\033[1;31m"
#define C_CYAN "\033[1;36m"
#define CLEAR_LINE "\r\033[K"

// Each stage has a color, used for its name and its share of the time bar.
static const struct {
  const char *name;
  const char *color;
} stages[] = {
    {"preprocess", "\033[38;5;177m"}, // violet
    {"compile", "\033[38;5;75m"},     // blue
    {"assemble", "\033[38;5;80m"},    // teal
    {"link", "\033[38;5;215m"},       // orange
};
enum { NUM_STAGES = sizeof stages / sizeof stages[0] };

static struct {
  bool enabled;
  bool color;
  bool unicode;
  bool animate;
  double stage_ms[NUM_STAGES]; // wall time per stage, summed over files

  // The live stage line.
  pthread_t spinner;
  pthread_mutex_t lock;
  pthread_cond_t wake;
  bool spinning;
  bool stop;
  const char *running;
  struct timespec started;
} R = {.lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER};

static const char *c(const char *code) { return R.color ? code : ""; }

static int stage_index(const char *name) {
  for (int i = 0; i < NUM_STAGES; i++)
    if (strcmp(stages[i].name, name) == 0)
      return i;
  return -1;
}

static const char *stage_color(const char *name) {
  int i = stage_index(name);
  return i < 0 ? "" : c(stages[i].color);
}

void report_init(bool enabled, bool color, bool animate) {
  R.enabled = enabled;
  R.color = color;
  R.unicode = locale_is_utf8();
  R.animate = enabled && color && animate && isatty(STDERR_FILENO) && !getenv("OCC_NO_ANIMATION");
}

bool report_enabled(void) { return R.enabled; }

static char *fmt_time(double ms) {
  if (ms < 1000)
    return format("%.2f ms", ms);
  return format("%.2f s", ms / 1000);
}

char *format_size(long bytes) {
  if (bytes < 0)
    return xstrdup("?");
  if (bytes < 1024)
    return format("%ld B", bytes);
  if (bytes < 1024 * 1024)
    return format("%.1f KiB", (double)bytes / 1024);
  return format("%.1f MiB", (double)bytes / (1024 * 1024));
}

// ---------------------------------------------------------------------------
// Live stage line
// ---------------------------------------------------------------------------

static double ms_since(const struct timespec *t0) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)(now.tv_sec - t0->tv_sec) * 1e3 + (double)(now.tv_nsec - t0->tv_nsec) / 1e6;
}

static void *spin(void *) {
  static const char *const frames_utf8[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
  static const char *const frames_ascii[] = {"|", "/", "-", "\\"};
  const char *const *frames = R.unicode ? frames_utf8 : frames_ascii;
  int nframes = R.unicode ? 10 : 4;

  pthread_mutex_lock(&R.lock);
  for (int frame = 0; !R.stop; frame++) {
    // Stay quiet for the first 40 ms: most stages finish before anyone could
    // see a spinner, and drawing one would only flicker.
    double ms = ms_since(&R.started);
    if (ms >= 40) {
      fprintf(stderr, CLEAR_LINE "    %s%s%s %s%-10s%s %s%10s%s", c(C_CYAN), frames[frame % nframes], c(C_RESET),
              stage_color(R.running), R.running, c(C_RESET), c(C_DIM), fmt_time(ms), c(C_RESET));
      fflush(stderr);
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 80 * 1000 * 1000;
    if (until.tv_nsec >= 1000000000) {
      until.tv_sec++;
      until.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&R.wake, &R.lock, &until);
  }
  pthread_mutex_unlock(&R.lock);
  return nullptr;
}

void report_stage_begin(const char *name) {
  if (!R.animate)
    return;
  R.running = name;
  R.stop = false;
  clock_gettime(CLOCK_MONOTONIC, &R.started);
  R.spinning = pthread_create(&R.spinner, nullptr, spin, nullptr) == 0;
}

static void stop_spinner(void) {
  if (!R.spinning)
    return;
  pthread_mutex_lock(&R.lock);
  R.stop = true;
  pthread_cond_signal(&R.wake);
  pthread_mutex_unlock(&R.lock);
  pthread_join(R.spinner, nullptr);
  R.spinning = false;
  fputs(CLEAR_LINE, stderr);
}

// ---------------------------------------------------------------------------
// Lines
// ---------------------------------------------------------------------------

void report_header(const char *opt_level, const char *target) {
  if (!R.enabled)
    return;
  const char *dot = R.unicode ? "·" : "|";
  fprintf(stderr, "%s%socc%s%s " OCC_VERSION "%s %s %s %s C23 %s opt=%s%s%s\n", c(C_BOLD), c(C_CYAN), c(C_RESET),
          c(C_BOLD), c(C_RESET), dot, target, dot, dot, c(C_CYAN), opt_level, c(C_RESET));
}

void report_file(const char *path) {
  if (R.enabled)
    fprintf(stderr, "  %s%s%s\n", c(C_BOLD), path, c(C_RESET));
}

void report_stage(const char *name, StageStatus status, Duration time, const char *detail) {
  stop_spinner();
  int i = stage_index(name);
  if (i >= 0)
    R.stage_ms[i] += time.wall_ms;
  if (!R.enabled)
    return;
  static const char *labels[] = {"OK  ", "WARN", "ERR "};
  static const char *marks[] = {"✓", "!", "✗"};
  const char *colors[] = {C_GREEN, C_YELLOW, C_RED};
  if (R.animate && R.unicode) {
    fprintf(stderr, "    %s%s%s %s%-10s%s %s%s%s %10s cpu %10s wall  %s%s%s\n", c(colors[status]), marks[status],
            c(C_RESET), stage_color(name), name, c(C_RESET), c(colors[status]), labels[status], c(C_RESET),
            fmt_time(time.cpu_ms), fmt_time(time.wall_ms), c(C_DIM), detail ? detail : "", c(C_RESET));
    return;
  }
  fprintf(stderr, "    %s%-11s%s %s%s%s %10s cpu %10s wall  %s%s%s\n", stage_color(name), name, c(C_RESET),
          c(colors[status]), labels[status], c(C_RESET), fmt_time(time.cpu_ms), fmt_time(time.wall_ms), c(C_DIM),
          detail ? detail : "", c(C_RESET));
}

void report_substage(const char *name, bool last, Duration time, const char *detail) {
  if (!R.enabled)
    return;
  const char *branch = R.unicode ? (last ? "└" : "├") : (last ? "`" : "|");
  const char *indent = R.animate && R.unicode ? "       " : "      ";
  fprintf(stderr, "%s%s%s %-11s%s %10s cpu                  %s%s%s\n", indent, c(C_DIM), branch, name, c(C_RESET),
          fmt_time(time.cpu_ms), c(C_DIM), detail ? detail : "", c(C_RESET));
}

void report_command(const char *cmdline) {
  if (R.enabled)
    fprintf(stderr, "      %s$ %s%s\n", c(C_DIM), cmdline, c(C_RESET));
}

void report_note(const char *fmt, ...) {
  if (!R.enabled)
    return;
  va_list ap;
  va_start(ap, fmt);
  fputs("  ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

// Where the time went: one colored segment per stage, with a legend.
static void print_time_bar(void) {
  double total = 0;
  for (int i = 0; i < NUM_STAGES; i++)
    total += R.stage_ms[i];
  if (total <= 0)
    return;
  enum { WIDTH = 40 };
  int cells[NUM_STAGES], used = 0, widest = 0;
  for (int i = 0; i < NUM_STAGES; i++) {
    cells[i] = (int)(R.stage_ms[i] / total * WIDTH + 0.5);
    if (R.stage_ms[i] > 0 && cells[i] == 0)
      cells[i] = 1; // every stage that ran stays visible
    used += cells[i];
    if (cells[i] > cells[widest])
      widest = i;
  }
  cells[widest] += WIDTH - used; // rounding goes to the largest segment

  fputs("    ", stderr);
  for (int i = 0; i < NUM_STAGES; i++) {
    fputs(c(stages[i].color), stderr);
    for (int k = 0; k < cells[i]; k++)
      fputs("█", stderr);
  }
  fputs(c(C_RESET), stderr);
  const char *sep = "";
  fputs("  ", stderr);
  for (int i = 0; i < NUM_STAGES; i++) {
    if (R.stage_ms[i] <= 0)
      continue;
    fprintf(stderr, "%s%s%s %s%.0f%%%s", sep, c(stages[i].color), stages[i].name, c(C_DIM),
            R.stage_ms[i] / total * 100, c(C_RESET));
    sep = "  ";
  }
  fputc('\n', stderr);
}

void report_summary(bool ok, const char *what, Duration total, int errors, int warnings) {
  stop_spinner();
  if (!R.enabled)
    return;
  const char *dot = R.unicode ? "·" : "-";
  const char *mark = ok ? (R.unicode ? "✓" : "OK") : (R.unicode ? "✗" : "FAILED");
  fprintf(stderr, "  %s%s%s ", c(ok ? C_GREEN : C_RED), mark, c(C_RESET));
  if (ok)
    fprintf(stderr, "%s%s%s in %s (cpu %s)", c(C_BOLD), what, c(C_RESET), fmt_time(total.wall_ms),
            fmt_time(total.cpu_ms));
  else
    fprintf(stderr, "%sbuild failed%s", c(C_BOLD), c(C_RESET));
  fprintf(stderr, " %s %s%d error%s%s, %s%d warning%s%s\n", dot, errors ? c(C_RED) : "", errors,
          errors == 1 ? "" : "s", c(C_RESET), warnings ? c(C_YELLOW) : "", warnings, warnings == 1 ? "" : "s",
          c(C_RESET));
  if (ok && R.animate && R.unicode)
    print_time_bar();
}

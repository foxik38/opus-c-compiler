// report.c - the pipeline status display (written to stderr).
#include "driver/report.h"

#include <stdarg.h>

#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_DIM "\033[2m"
#define C_GREEN "\033[1;32m"
#define C_YELLOW "\033[1;33m"
#define C_RED "\033[1;31m"
#define C_CYAN "\033[36m"

static struct {
  bool enabled;
  bool color;
  bool unicode;
} R;

static const char *c(const char *code) { return R.color ? code : ""; }

static bool locale_is_utf8(void) {
  const char *vars[] = {"LC_ALL", "LC_CTYPE", "LANG"};
  for (size_t i = 0; i < ARRAY_LEN(vars); i++) {
    const char *v = getenv(vars[i]);
    if (v && *v)
      return strstr(v, "UTF-8") || strstr(v, "utf8") || strstr(v, "UTF8") || strstr(v, "utf-8");
  }
  return false;
}

void report_init(bool enabled, bool color) {
  R.enabled = enabled;
  R.color = color;
  R.unicode = locale_is_utf8();
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

void report_header(const char *opt_level, const char *target) {
  if (!R.enabled)
    return;
  const char *dot = R.unicode ? "·" : "|";
  fprintf(stderr, "%socc " OCC_VERSION "%s %s %s %s C23 %s opt=%s%s%s\n", c(C_BOLD), c(C_RESET), dot, target, dot,
          dot, c(C_CYAN), opt_level, c(C_RESET));
}

void report_file(const char *path) {
  if (R.enabled)
    fprintf(stderr, "  %s%s%s\n", c(C_BOLD), path, c(C_RESET));
}

void report_stage(const char *name, StageStatus status, Duration time, const char *detail) {
  if (!R.enabled)
    return;
  static const char *labels[] = {"OK  ", "WARN", "ERR "};
  const char *colors[] = {C_GREEN, C_YELLOW, C_RED};
  fprintf(stderr, "    %-11s %s%s%s %10s cpu %10s wall  %s%s%s\n", name, c(colors[status]), labels[status],
          c(C_RESET), fmt_time(time.cpu_ms), fmt_time(time.wall_ms), c(C_DIM), detail ? detail : "", c(C_RESET));
}

void report_substage(const char *name, bool last, Duration time, const char *detail) {
  if (!R.enabled)
    return;
  const char *branch = R.unicode ? (last ? "└" : "├") : (last ? "`" : "|");
  fprintf(stderr, "      %s%s %-11s%s %10s cpu                  %s%s%s\n", c(C_DIM), branch, name, c(C_RESET),
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

void report_summary(bool ok, const char *what, Duration total, int errors, int warnings) {
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
  fprintf(stderr, " %s %d error%s, %d warning%s\n", dot, errors, errors == 1 ? "" : "s", warnings,
          warnings == 1 ? "" : "s");
}

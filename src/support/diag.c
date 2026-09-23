// diag.c - diagnostics buffering, formatting and warning configuration.
#include "support/diag.h"

#include "support/strbuf.h"

#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_RED "\033[1;31m"
#define C_MAGENTA "\033[1;35m"
#define C_CYAN "\033[1;36m"
#define C_GREEN "\033[1;32m"
#define C_DIM "\033[2m"

typedef struct {
  const char *name;
  const char *desc;
} WarningInfo;

static const WarningInfo warning_info[W_COUNT] = {
#define X(id, name, desc) [id] = {name, desc},
    WARNING_LIST(X)
#undef X
};

static struct {
  bool color;
  bool werror;
  bool suppress_warnings;
  bool disabled[W_COUNT];
  int errors;
  int warnings;
  StrBuf buffer;
  jmp_buf *recovery;
} diag;

void diag_configure(bool color, bool werror, bool suppress_warnings) {
  diag.color = color;
  diag.werror = werror;
  diag.suppress_warnings = suppress_warnings;
}

bool diag_use_color(void) { return diag.color; }

bool diag_set_warning(const char *name, bool enabled) {
  if (strcmp(name, "all") == 0 || strcmp(name, "extra") == 0) {
    for (int i = 0; i < W_COUNT; i++)
      diag.disabled[i] = !enabled;
    return true;
  }
  for (int i = 0; i < W_COUNT; i++) {
    if (strcmp(warning_info[i].name, name) == 0) {
      diag.disabled[i] = !enabled;
      return true;
    }
  }
  return false;
}

bool diag_warning_enabled(WarningId id) {
  return !diag.suppress_warnings && id < W_COUNT && !diag.disabled[id];
}

void diag_print_warning_list(FILE *out) {
  for (int i = 0; i < W_COUNT; i++)
    fprintf(out, "  -W%-28s %s\n", warning_info[i].name, warning_info[i].desc);
}

static const char *paint(const char *color) { return diag.color ? color : ""; }

// Prints the source line containing loc with a caret under the range.
static void print_excerpt(StrBuf *sb, const SrcLoc *loc, const char *color) {
  const char *contents = loc->file->contents;
  const char *start = loc->pos;
  while (start > contents && start[-1] != '\n')
    start--;
  const char *end = loc->pos;
  while (*end && *end != '\n')
    end++;

  sb_printf(sb, " %5d | ", loc->line);
  sb_append(sb, start, (size_t)(end - start));
  sb_puts(sb, "\n       | ");

  for (const char *p = start; p < loc->pos; p++) {
    if ((*p & 0xC0) == 0x80) // UTF-8 continuation byte occupies no column
      continue;
    sb_putc(sb, *p == '\t' ? '\t' : ' ');
  }
  sb_puts(sb, paint(color));
  sb_putc(sb, '^');
  const char *range_end = MIN(loc->pos + MAX(loc->len, 1), end);
  for (const char *p = loc->pos + 1; p < range_end; p++)
    if ((*p & 0xC0) != 0x80)
      sb_putc(sb, '~');
  sb_puts(sb, paint(C_RESET));
  sb_putc(sb, '\n');
}

static int column_of(const SrcLoc *loc) {
  int col = 1;
  for (const char *p = loc->pos; p > loc->file->contents && p[-1] != '\n'; p--)
    if ((p[-1] & 0xC0) != 0x80)
      col++;
  return col;
}

bool diag_vreport(DiagLevel level, WarningId wid, const SrcLoc *loc, const char *fmt, va_list ap) {
  if (level == DIAG_WARNING) {
    if (!diag_warning_enabled(wid) || (loc && loc->file && loc->file->is_system))
      return false;
    if (diag.werror)
      level = DIAG_ERROR;
  }

  static const char *labels[] = {"error", "warning", "note"};
  static const char *colors[] = {C_RED, C_MAGENTA, C_CYAN};

  StrBuf *sb = &diag.buffer;
  sb_puts(sb, paint(C_BOLD));
  if (loc && loc->file)
    sb_printf(sb, "%s:%d:%d: ", loc->file->display_name, loc->line, column_of(loc));
  else
    sb_puts(sb, "occ: ");
  sb_printf(sb, "%s%s:%s%s ", paint(colors[level]), labels[level], paint(C_RESET), paint(C_BOLD));
  sb_vprintf(sb, fmt, ap);
  sb_puts(sb, paint(C_RESET));
  if (wid < W_COUNT)
    sb_printf(sb, " [-W%s%s]", diag.werror ? "error=" : "", warning_info[wid].name);
  sb_putc(sb, '\n');
  if (loc && loc->file && loc->pos)
    print_excerpt(sb, loc, level == DIAG_NOTE ? C_CYAN : C_GREEN);

  if (level == DIAG_ERROR)
    diag.errors++;
  else if (level == DIAG_WARNING)
    diag.warnings++;
  return true;
}

bool diag_report(DiagLevel level, WarningId wid, const SrcLoc *loc, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bool shown = diag_vreport(level, wid, loc, fmt, ap);
  va_end(ap);
  return shown;
}

void fatal(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_vreport(DIAG_ERROR, W_NONE, nullptr, fmt, ap);
  va_end(ap);
  diag_abort();
}

void diag_set_recovery(jmp_buf *jb) { diag.recovery = jb; }

void diag_abort(void) {
  if (diag.recovery)
    longjmp(*diag.recovery, 1);
  diag_flush();
  exit(1);
}

int diag_error_count(void) { return diag.errors; }
int diag_warning_count(void) { return diag.warnings; }

void diag_flush(void) {
  if (diag.buffer.len) {
    fputs(diag.buffer.data, stderr);
    fflush(stderr);
  }
  sb_clear(&diag.buffer);
}

// emit.c - the assembly line buffer and register naming.
#include "codegen/cg.h"

CodegenState cg;

static void add_line(const char *prefix, const char *fmt, va_list ap) {
  char *body = vformat(fmt, ap);
  if (*prefix) {
    char *line = format("%s%s", prefix, body);
    free(body);
    body = line;
  }
  vec_push(&cg.lines, body);
}

void emit(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  add_line("\t", fmt, ap);
  va_end(ap);
}

void emit_label(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *name = vformat(fmt, ap);
  va_end(ap);
  vec_push(&cg.lines, format("%s:", name));
  free(name);
}

void emit_raw(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  add_line("", fmt, ap);
  va_end(ap);
}

int new_cg_label(void) { return ++cg.label_seq; }

void flush_lines(FILE *out) {
  for (size_t i = 0; i < cg.lines.len; i++) {
    const char *line = cg.lines.data[i];
    if (!line)
      continue; // removed by the peephole optimizer
    fputs(line, out);
    fputc('\n', out);
    if (line[0] == '\t' && line[1] != '.')
      cg.stats->instructions++;
  }
}

const char *reg_ax(int size) {
  switch (size) {
  case 1: return "%al";
  case 2: return "%ax";
  case 4: return "%eax";
  default: return "%rax";
  }
}

const char *reg_di(int size) {
  switch (size) {
  case 1: return "%dil";
  case 2: return "%di";
  case 4: return "%edi";
  default: return "%rdi";
  }
}

const char *arg_reg(int idx, int size) {
  static const char *regs[][4] = {
      {"%dil", "%di", "%edi", "%rdi"}, {"%sil", "%si", "%esi", "%rsi"}, {"%dl", "%dx", "%edx", "%rdx"},
      {"%cl", "%cx", "%ecx", "%rcx"},  {"%r8b", "%r8w", "%r8d", "%r8"}, {"%r9b", "%r9w", "%r9d", "%r9"},
  };
  int col = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
  return regs[idx][col];
}

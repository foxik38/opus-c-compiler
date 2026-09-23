// peephole.c - local cleanups on the generated instruction list (-o prod).
//
// Every rewrite looks at one or two adjacent instructions and is valid
// regardless of the surrounding code, given the code generator's own
// conventions (flags are never live across instructions it does not pair,
// %rax is dead after a value has been reloaded into it, ...).
#include <ctype.h>

#include "codegen/cg.h"
#include "support/hashmap.h"

typedef struct {
  StrVec *v;
  int removed;
  bool changed;
} Peephole;

static bool is_insn(const char *l) { return l && l[0] == '\t' && l[1] != '.'; }

static bool is_label(const char *l) {
  size_t n = l ? strlen(l) : 0;
  return n > 1 && l[0] != '\t' && l[0] != '#' && l[n - 1] == ':';
}

static int next_live(StrVec *v, int i) {
  for (size_t k = (size_t)i + 1; k < v->len; k++)
    if (v->data[k])
      return (int)k;
  return -1;
}

static void kill(Peephole *p, int i) {
  if (is_insn(p->v->data[i]))
    p->removed++;
  p->v->data[i] = nullptr;
  p->changed = true;
}

static void replace(Peephole *p, int i, char *line) {
  p->v->data[i] = line;
  p->changed = true;
}

static bool eq(const char *a, const char *b) { return a && strcmp(a, b) == 0; }

static bool reads_flags(const char *l) {
  if (!is_insn(l))
    return false;
  const char *m = l + 1;
  return (m[0] == 'j' && !starts_with(m, "jmp")) || starts_with(m, "set") || starts_with(m, "cmov") ||
         starts_with(m, "adc") || starts_with(m, "sbb");
}

// Splits "\tOP A, B" into its parts. Returns false for other shapes.
static bool split2(const char *l, char *op, char *a, char *b) {
  if (!is_insn(l))
    return false;
  const char *sp = strchr(l + 1, ' ');
  if (!sp)
    return false;
  const char *comma = nullptr;
  int depth = 0;
  for (const char *c = sp + 1; *c; c++) {
    if (*c == '(')
      depth++;
    else if (*c == ')')
      depth--;
    else if (*c == ',' && depth == 0) {
      if (comma)
        return false;
      comma = c;
    }
  }
  if (!comma || comma[1] != ' ')
    return false;
  snprintf(op, 16, "%.*s", (int)(sp - l - 1), l + 1);
  snprintf(a, 128, "%.*s", (int)(comma - sp - 1), sp + 1);
  snprintf(b, 128, "%s", comma + 2);
  return true;
}

static bool is_reg(const char *s) { return s[0] == '%'; }
static bool is_mem(const char *s) { return s[0] != '%' && s[0] != '$'; }

static void rewrite(Peephole *p, int i) {
  StrVec *v = p->v;
  char *l = v->data[i];
  int j = next_live(v, i);
  char *n = j >= 0 ? v->data[j] : nullptr;
  char op[16], a[128], b[128], op2[16], a2[128], b2[128];

  // push %rax; pop %reg  =>  mov %rax, %reg
  if (eq(l, "\tpush %rax") && n && starts_with(n, "\tpop ")) {
    if (eq(n + 5, "%rax")) {
      kill(p, i);
    } else {
      replace(p, i, format("\tmov %%rax, %s", n + 5));
    }
    kill(p, j);
    return;
  }

  if (eq(l, "\tmov %rax, %rax")) {
    kill(p, i);
    return;
  }

  // Jump to the immediately following label.
  if (starts_with(l, "\tjmp ") && l[5] != '*') {
    for (int k = j; k >= 0 && is_label(v->data[k]); k = next_live(v, k)) {
      size_t len = strlen(v->data[k]) - 1;
      if (strlen(l + 5) == len && strncmp(v->data[k], l + 5, len) == 0) {
        kill(p, i);
        return;
      }
    }
  }

  // Code after an unconditional transfer is unreachable up to the next label.
  if (eq(l, "\tret") || eq(l, "\tud2") || starts_with(l, "\tjmp ")) {
    for (int k = j; k >= 0 && is_insn(v->data[k]); k = next_live(v, k))
      kill(p, k);
    return;
  }

  // cmp $0, %reg  =>  test %reg, %reg (identical flags)
  if (starts_with(l, "\tcmp $0, %")) {
    const char *reg = l + 9;
    replace(p, i, format("\ttest %s, %s", reg, reg));
    return;
  }

  // mov $0, %eax  =>  xor %eax, %eax (when nothing reads the flags next)
  if ((eq(l, "\tmov $0, %eax") || eq(l, "\tmov $0, %rax")) && !reads_flags(n)) {
    replace(p, i, xstrdup("\txor %eax, %eax"));
    return;
  }

  if (!split2(l, op, a, b) || !n)
    return;
  bool pair = split2(n, op2, a2, b2);

  // Store followed by a reload of the same location.
  if (pair && is_reg(a) && is_mem(b) && strcmp(op, op2) == 0 &&
      (strcmp(op, "mov") == 0 || strcmp(op, "movsd") == 0 || strcmp(op, "movss") == 0) && strcmp(a, b2) == 0 &&
      strcmp(b, a2) == 0) {
    kill(p, j);
    return;
  }

  // mov A, B; mov B, A  =>  mov A, B
  if (pair && strcmp(op, "mov") == 0 && strcmp(op2, "mov") == 0 && is_reg(a) && is_reg(b) && strcmp(a, b2) == 0 &&
      strcmp(b, a2) == 0) {
    kill(p, j);
    return;
  }

  // lea M, %rax; OP (%rax), %eax|%rax  =>  OP M, %eax|%rax
  if (strcmp(op, "lea") == 0 && strcmp(b, "%rax") == 0 && !strstr(a, "%rax") && pair &&
      strcmp(a2, "(%rax)") == 0 && (strcmp(b2, "%eax") == 0 || strcmp(b2, "%rax") == 0) &&
      (strcmp(op2, "mov") == 0 || strcmp(op2, "movsbl") == 0 || strcmp(op2, "movzbl") == 0 ||
       strcmp(op2, "movswl") == 0 || strcmp(op2, "movzwl") == 0)) {
    replace(p, j, format("\t%s %s, %s", op2, a, b2));
    kill(p, i);
    return;
  }
}

// Collects every ".L..." name mentioned outside label definitions.
static void collect_label_refs(StrVec *v, HashMap *refs) {
  for (size_t i = 0; i < v->len; i++) {
    const char *l = v->data[i];
    if (!l || is_label(l))
      continue;
    for (const char *p = strstr(l, ".L"); p; p = strstr(p + 2, ".L")) {
      const char *e = p;
      while (*e && (isalnum((unsigned char)*e) || *e == '.' || *e == '_'))
        e++;
      hashmap_put2(refs, p, (int)(e - p), (void *)1);
    }
  }
}

int peephole(StrVec *lines) {
  Peephole p = {.v = lines};
  for (int iter = 0; iter < 8; iter++) {
    p.changed = false;
    for (size_t i = 0; i < lines->len; i++)
      if (is_insn(lines->data[i]))
        rewrite(&p, (int)i);
    if (!p.changed)
      break;
  }

  // Drop local labels nobody refers to (keeps the listing readable).
  HashMap refs = {};
  collect_label_refs(lines, &refs);
  for (size_t i = 0; i < lines->len; i++) {
    const char *l = lines->data[i];
    if (is_label(l) && starts_with(l, ".L") && !hashmap_get2(&refs, l, (int)strlen(l) - 1))
      lines->data[i] = nullptr;
  }
  return p.removed;
}

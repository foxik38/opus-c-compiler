// check.c - diagnostics that need to look at whole constructs.
#include "parse/parser.h"

// ---------------------------------------------------------------------------
// Unused and uninitialized variables
// ---------------------------------------------------------------------------

void check_unused_locals(Scope *scope) {
  for (size_t i = 0; i < scope->locals.len; i++) {
    Obj *v = scope->locals.data[i];
    if (v->is_param || v->maybe_unused || !v->tok || !v->name[0])
      continue;
    if (v->refs == 0) {
      warn_tok(W_UNUSED_VARIABLE, v->tok, "unused variable '%s'", v->name);
      continue;
    }
    if (v->refs == v->lhs_refs && !v->addr_taken && is_scalar(v->ty)) {
      warn_tok(W_UNUSED_VARIABLE, v->tok, "variable '%s' set but not used", v->name);
      continue;
    }
    if (is_scalar(v->ty) && !v->has_init && v->writes == 0 && v->first_read)
      warn_tok(W_UNINITIALIZED, v->first_read, "variable '%s' is uninitialized when used here", v->name);
  }
}

// ---------------------------------------------------------------------------
// Control flow: can execution fall off the end of a statement?
// ---------------------------------------------------------------------------

static bool jumps_to(Node *n, int label) {
  for (; n; n = n->next) {
    if (n->kind == ND_GOTO && n->label_id == label)
      return true;
    if (n->kind == ND_BLOCK && jumps_to(n->body, label))
      return true;
    if (jumps_to(n->then, label) || jumps_to(n->els, label))
      return true;
    if ((n->kind == ND_LABEL || n->kind == ND_CASE) && jumps_to(n->lhs, label))
      return true;
  }
  return false;
}

static bool is_const_true(Node *cond) { return !cond || (is_const_expr(cond) && is_integer(cond->ty) && eval_int(cond)); }

bool is_noreturn_call(Node *n) {
  static const char *const known[] = {
      "exit",          "_Exit",  "abort", "quick_exit", "longjmp", "siglongjmp", "_longjmp",
      "__assert_fail", "err",    "errx",  "verr",       "verrx",   "pthread_exit", "thrd_exit",
  };
  if (n->kind != ND_EXPR_STMT)
    return false;
  Node *e = n->lhs;
  if (e->kind == ND_CAST && e->ty->kind == TY_VOID)
    e = e->lhs;
  if (e->kind == ND_UNREACHABLE)
    return true;
  if (e->kind != ND_FUNCALL || e->lhs->kind != ND_ADDR || e->lhs->lhs->kind != ND_VAR)
    return false;
  Obj *fn = e->lhs->lhs->var;
  if (fn->is_noreturn)
    return true;
  for (size_t i = 0; i < ARRAY_LEN(known); i++)
    if (strcmp(fn->name, known[i]) == 0)
      return true;
  return false;
}

bool stmt_falls_through(Node *n) {
  if (!n)
    return true;
  switch (n->kind) {
  case ND_RETURN:
  case ND_GOTO:
  case ND_UNREACHABLE:
    return false;
  case ND_EXPR_STMT:
    return !is_noreturn_call(n);
  case ND_BLOCK: {
    bool reachable = true;
    for (Node *s = n->body; s; s = s->next) {
      if (s->kind == ND_LABEL || s->kind == ND_CASE)
        reachable = true; // reachable through a jump
      if (reachable)
        reachable = stmt_falls_through(s);
    }
    return reachable;
  }
  case ND_LABEL:
  case ND_CASE:
    return stmt_falls_through(n->lhs);
  case ND_IF:
    return !n->els || stmt_falls_through(n->then) || stmt_falls_through(n->els);
  case ND_FOR:
    if (is_const_true(n->cond))
      return jumps_to(n->then, n->brk_label);
    return true;
  case ND_DO:
    if (is_const_true(n->cond))
      return jumps_to(n->then, n->brk_label);
    return stmt_falls_through(n->then) || jumps_to(n->then, n->brk_label) || jumps_to(n->then, n->cont_label);
  case ND_SWITCH:
    if (!n->default_case)
      return true;
    return stmt_falls_through(n->then) || jumps_to(n->then, n->brk_label);
  default:
    return true;
  }
}

// ---------------------------------------------------------------------------
// printf/scanf format strings
// ---------------------------------------------------------------------------

typedef enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T, LEN_BIG_L } LengthMod;

static int format_index(const char *name) {
  if (!strcmp(name, "printf") || !strcmp(name, "scanf"))
    return 0;
  if (!strcmp(name, "snprintf"))
    return 2;
  return 1; // fprintf, sprintf, dprintf, fscanf, sscanf
}

static const char *int_type_name(LengthMod len, bool is_unsigned) {
  static const char *names[][2] = {
      [LEN_NONE] = {"int", "unsigned int"},    [LEN_HH] = {"char", "unsigned char"},
      [LEN_H] = {"short", "unsigned short"},   [LEN_L] = {"long", "unsigned long"},
      [LEN_LL] = {"long long", "unsigned long long"}, [LEN_J] = {"intmax_t", "uintmax_t"},
      [LEN_Z] = {"ssize_t", "size_t"},         [LEN_T] = {"ptrdiff_t", "ptrdiff_t"},
      [LEN_BIG_L] = {"long long", "unsigned long long"},
  };
  return names[len][is_unsigned];
}

static int int_size(LengthMod len, bool for_scanf) {
  switch (len) {
  case LEN_HH: return for_scanf ? 1 : 4; // printf arguments are promoted
  case LEN_H: return for_scanf ? 2 : 4;
  case LEN_L: case LEN_LL: case LEN_J: case LEN_Z: case LEN_T: case LEN_BIG_L: return 8;
  default: return 4;
  }
}

static Node *strip_casts(Node *n) {
  while (n->kind == ND_CAST && n->is_implicit)
    n = n->lhs;
  return n;
}

static void mismatch(Node *arg, const char *expected) {
  warn_tok(W_FORMAT, arg->tok, "format specifies type '%s' but the argument has type '%s'", expected,
           type_name(strip_casts(arg)->ty));
}

static void check_printf_arg(Node *arg, char conv, LengthMod len) {
  Type *ty = strip_casts(arg)->ty;
  if (arg->ty->kind == TY_DOUBLE && ty->kind == TY_FLOAT)
    ty = arg->ty; // float was promoted to double

  switch (conv) {
  case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
    bool is_unsigned = conv != 'd' && conv != 'i';
    Type *promoted = is_integer(ty) ? integer_promote(ty) : ty;
    if (!is_integer(ty) || promoted->size != int_size(len, false))
      mismatch(arg, int_type_name(len, is_unsigned));
    return;
  }
  case 'c':
    if (!is_integer(ty))
      mismatch(arg, len == LEN_L ? "wint_t" : "int");
    return;
  case 's':
    if (ty->kind != TY_PTR || !is_integer(ty->base) || ty->base->size != (len == LEN_L ? 4 : 1))
      mismatch(arg, len == LEN_L ? "wchar_t *" : "char *");
    return;
  case 'p':
    if (!is_pointer_like(ty))
      mismatch(arg, "void *");
    return;
  case 'n':
    if (ty->kind != TY_PTR || !is_integer(ty->base))
      mismatch(arg, "int *");
    return;
  case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
    if (!is_flonum(ty))
      mismatch(arg, len == LEN_BIG_L ? "long double" : "double");
    return;
  default:
    return;
  }
}

static void check_scanf_arg(Node *arg, char conv, LengthMod len) {
  Type *ty = strip_casts(arg)->ty;
  if (ty->kind != TY_PTR) {
    warn_tok(W_FORMAT, arg->tok, "format specifies a pointer but the argument has type '%s'", type_name(ty));
    return;
  }
  Type *base = ty->base;
  switch (conv) {
  case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'n': {
    bool is_unsigned = conv != 'd' && conv != 'i' && conv != 'n';
    if (!is_integer(base) || base->size != int_size(len, true))
      warn_tok(W_FORMAT, arg->tok, "format specifies type '%s *' but the argument has type '%s'",
               int_type_name(len, is_unsigned), type_name(ty));
    return;
  }
  case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
    int want = len == LEN_L || len == LEN_BIG_L ? 8 : 4;
    if (!is_flonum(base) || base->size != want)
      warn_tok(W_FORMAT, arg->tok, "format specifies type '%s *' but the argument has type '%s'",
               want == 8 ? "double" : "float", type_name(ty));
    return;
  }
  case 's': case 'c': case '[':
    if (!is_integer(base) || base->size != 1)
      warn_tok(W_FORMAT, arg->tok, "format specifies type 'char *' but the argument has type '%s'",
               type_name(ty));
    return;
  default:
    return;
  }
}

void check_format_call(Node *call, const char *name) {
  bool is_scanf = strstr(name, "scanf") != nullptr;
  int fmt_index = format_index(name);

  Node *arg = call->args;
  for (int i = 0; i < fmt_index && arg; i++)
    arg = arg->next;
  if (!arg)
    return;
  Node *fmt = strip_casts(arg);
  if (fmt->kind != ND_ADDR || fmt->lhs->kind != ND_VAR || !fmt->lhs->var->is_string_literal ||
      fmt->lhs->ty->base->size != 1)
    return; // only literal formats can be checked
  const char *s = fmt->lhs->var->init_data;
  Node *next = arg->next;

  for (const char *p = s; *p; p++) {
    if (*p != '%')
      continue;
    p++;
    if (*p == '%')
      continue;
    if (!*p) {
      warn_tok(W_FORMAT, arg->tok, "incomplete format specifier");
      return;
    }

    bool suppress = false;
    if (is_scanf && *p == '*') {
      suppress = true;
      p++;
    }
    // Flags, width and precision (printf's '*' consumes an int argument).
    while (!is_scanf && strchr("-+ #0'", *p))
      p++;
    for (int part = 0; part < 2; part++) {
      if (part == 1) {
        if (*p != '.' || is_scanf)
          break;
        p++;
      }
      if (*p == '*' && !is_scanf) {
        if (!next) {
          warn_tok(W_FORMAT, arg->tok, "'*' specified field width/precision is missing a matching argument");
          return;
        }
        if (!is_integer(strip_casts(next)->ty))
          mismatch(next, "int");
        next = next->next;
        p++;
      } else {
        while (*p >= '0' && *p <= '9')
          p++;
      }
    }

    LengthMod len = LEN_NONE;
    if (p[0] == 'h' && p[1] == 'h') {
      len = LEN_HH;
      p += 2;
    } else if (p[0] == 'l' && p[1] == 'l') {
      len = LEN_LL;
      p += 2;
    } else if (*p == 'h' || *p == 'l' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'L' || *p == 'q') {
      len = *p == 'h' ? LEN_H : *p == 'l' ? LEN_L : *p == 'j' ? LEN_J : *p == 'z' ? LEN_Z
            : *p == 't' ? LEN_T : *p == 'q' ? LEN_LL : LEN_BIG_L;
      p++;
    }

    char conv = *p;
    if (conv == 'm')
      continue; // glibc: strerror(errno), no argument
    if (!strchr("diuoxXcspnfFeEgGaA[", conv)) {
      warn_tok(W_FORMAT, arg->tok, "invalid conversion specifier '%c'", conv ? conv : '?');
      return;
    }
    if (conv == '[') {
      p++;
      if (*p == '^')
        p++;
      if (*p == ']')
        p++;
      while (*p && *p != ']')
        p++;
      if (!*p)
        return;
    }
    if (suppress)
      continue;
    if (!next) {
      warn_tok(W_FORMAT, arg->tok, "more '%%' conversions than data arguments");
      return;
    }
    if (is_scanf)
      check_scanf_arg(next, conv, len);
    else
      check_printf_arg(next, conv, len);
    next = next->next;
  }

  if (next && !is_scanf)
    warn_tok(W_FORMAT, next->tok, "data argument not used by format string");
}

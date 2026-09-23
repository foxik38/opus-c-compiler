// macro.c - macro definitions and expansion.
//
// Expansion follows Dave Prosser's hideset algorithm: every token carries the
// set of macro names that must not be expanded again at that point, which
// makes recursive macros terminate exactly as the standard requires.
#include <time.h>

#include "preproc/pp_internal.h"
#include "support/hashmap.h"
#include "support/strbuf.h"

struct Hideset {
  Hideset *next;
  char *name;
};

typedef struct MacroParam MacroParam;
struct MacroParam {
  MacroParam *next;
  char *name;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
  MacroArg *next;
  char *name;
  bool is_va_args;
  Token *tok;      // unexpanded argument tokens (EOF-terminated)
  Token *expanded; // fully expanded tokens, computed on demand
};

typedef Token *MacroHandler(Token *tok);

typedef struct {
  char *name;
  bool is_objlike;
  MacroParam *params;
  char *va_args_name; // non-null for variadic macros
  Token *body;
  Token *def;         // name token of the definition (for diagnostics)
  MacroHandler *handler;
} Macro;

static HashMap macros;
static int counter_value;

// ---------------------------------------------------------------------------
// Hidesets
// ---------------------------------------------------------------------------

static Hideset *new_hideset(char *name) {
  Hideset *hs = NEW(Hideset);
  hs->name = name;
  return hs;
}

static bool hideset_contains(const Hideset *hs, const char *s, int len) {
  for (; hs; hs = hs->next)
    if ((int)strlen(hs->name) == len && strncmp(hs->name, s, (size_t)len) == 0)
      return true;
  return false;
}

static Hideset *hideset_union(Hideset *a, Hideset *b) {
  Hideset head = {};
  Hideset *cur = &head;
  for (; a; a = a->next)
    cur = cur->next = new_hideset(a->name);
  cur->next = b;
  return head.next;
}

static Hideset *hideset_intersection(Hideset *a, Hideset *b) {
  Hideset head = {};
  Hideset *cur = &head;
  for (; a; a = a->next)
    if (hideset_contains(b, a->name, (int)strlen(a->name)))
      cur = cur->next = new_hideset(a->name);
  return head.next;
}

static Token *add_hideset(Token *tok, Hideset *hs) {
  Token head = {};
  Token *cur = &head;
  for (; tok; tok = tok->next) {
    Token *t = tok_copy(tok);
    t->hideset = hideset_union(t->hideset, hs);
    cur = cur->next = t;
  }
  return head.next;
}

// ---------------------------------------------------------------------------
// Token list utilities
// ---------------------------------------------------------------------------

// Returns a copy of list a (up to its EOF) followed by list b.
Token *pp_append(Token *a, Token *b) {
  if (a->kind == TK_EOF)
    return b;
  Token head = {};
  Token *cur = &head;
  for (; a->kind != TK_EOF; a = a->next)
    cur = cur->next = tok_copy(a);
  cur->next = b;
  return head.next;
}

// Copies the tokens up to the end of the current line into a new list.
Token *pp_copy_line(Token **rest, Token *tok) {
  Token head = {};
  Token *cur = &head;
  for (; !tok->at_bol; tok = tok->next)
    cur = cur->next = tok_copy(tok);
  cur->next = tok_new_eof(tok);
  *rest = tok;
  return head.next;
}

// Tokenizes synthetic text (stringizing, pasting, builtins) as if it had been
// spelled at tmpl's position.
Token *pp_tokenize_text(const char *text, const Token *tmpl) {
  SourceFile *file = source_new(tmpl->file ? tmpl->file->path : "<builtin>", xstrdup(text));
  if (tmpl->file) {
    file->display_name = tmpl->file->display_name;
    file->is_system = tmpl->file->is_system;
  }
  Token *tok = tokenize(file);
  for (Token *t = tok; t; t = t->next) {
    t->line = tmpl->line;
    t->at_bol = t->kind == TK_EOF; // synthetic tokens never start a directive
  }
  return tok;
}

static char *quote_string(const char *s) {
  StrBuf sb = {};
  sb_putc(&sb, '"');
  for (; *s; s++) {
    if (*s == '\\' || *s == '"')
      sb_putc(&sb, '\\');
    sb_putc(&sb, *s);
  }
  sb_putc(&sb, '"');
  return sb.data;
}

Token *pp_new_str_token(const char *str, const Token *tmpl) {
  return pp_tokenize_text(quote_string(str), tmpl);
}

static Token *new_num_token(int64_t val, const Token *tmpl) {
  return pp_tokenize_text(format("%lld", (long long)val), tmpl);
}

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

static Macro *find_macro(const Token *tok) {
  if (tok->kind != TK_IDENT)
    return nullptr;
  return hashmap_get2(&macros, tok->loc, tok->len);
}

bool macro_is_defined(const Token *name) { return find_macro(name) != nullptr; }

static Macro *add_macro(char *name, bool is_objlike, Token *body) {
  Macro *m = NEW(Macro);
  m->name = name;
  m->is_objlike = is_objlike;
  m->body = body;
  hashmap_put(&macros, name, m);
  return m;
}

void macro_undef(Token *name) { hashmap_delete2(&macros, name->loc, name->len); }

static MacroParam *read_macro_params(Token **rest, Token *tok, char **va_args_name) {
  MacroParam head = {};
  MacroParam *cur = &head;
  while (!tok_equal(tok, ")")) {
    if (cur != &head)
      tok = tok_skip(tok, ",");
    if (tok_equal(tok, "...")) {
      *va_args_name = "__VA_ARGS__";
      *rest = tok_skip(tok->next, ")");
      return head.next;
    }
    if (tok->kind != TK_IDENT)
      error_tok(tok, "expected parameter name in macro definition");
    if (tok_equal(tok->next, "...")) { // GNU named variadic parameter: args...
      *va_args_name = tok_text(tok);
      *rest = tok_skip(tok->next->next, ")");
      return head.next;
    }
    for (MacroParam *p = head.next; p; p = p->next)
      if ((int)strlen(p->name) == tok->len && strncmp(p->name, tok->loc, (size_t)tok->len) == 0)
        error_tok(tok, "duplicate macro parameter '%.*s'", tok->len, tok->loc);
    MacroParam *p = NEW(MacroParam);
    p->name = tok_text(tok);
    cur = cur->next = p;
    tok = tok->next;
  }
  *rest = tok->next;
  return head.next;
}

static bool same_body(const Token *a, const Token *b) {
  for (; a->kind != TK_EOF && b->kind != TK_EOF; a = a->next, b = b->next)
    if (a->len != b->len || memcmp(a->loc, b->loc, (size_t)a->len) != 0 ||
        a->has_space != b->has_space)
      return false;
  return a->kind == TK_EOF && b->kind == TK_EOF;
}

static bool same_params(const MacroParam *a, const MacroParam *b) {
  for (; a && b; a = a->next, b = b->next)
    if (strcmp(a->name, b->name) != 0)
      return false;
  return !a && !b;
}

// Parses the rest of a #define line (tok is the macro name).
Token *macro_define(Token *tok) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "macro name must be an identifier");
  Token *name_tok = tok;
  char *name = tok_text(tok);
  if (strcmp(name, "defined") == 0)
    error_tok(tok, "'defined' cannot be used as a macro name");
  tok = tok->next;

  bool is_objlike = true;
  MacroParam *params = nullptr;
  char *va_args_name = nullptr;
  if (!tok->has_space && tok_equal(tok, "(")) {
    is_objlike = false;
    params = read_macro_params(&tok, tok->next, &va_args_name);
  }
  Token *body = pp_copy_line(&tok, tok);

  if (!is_objlike)
    for (Token *t = body; t->kind != TK_EOF; t = t->next)
      if (tok_equal(t, "#")) {
        bool ok = t->next->kind == TK_IDENT &&
                  (tok_equal(t->next, "__VA_ARGS__") || tok_equal(t->next, "__VA_OPT__"));
        for (MacroParam *p = params; p && !ok; p = p->next)
          ok = (int)strlen(p->name) == t->next->len && !strncmp(p->name, t->next->loc, (size_t)t->next->len);
        if (!ok && !(va_args_name && tok_equal(t->next, va_args_name)))
          error_tok(t, "'#' is not followed by a macro parameter");
      }
  if (body->kind != TK_EOF && tok_equal(body, "##"))
    error_tok(body, "'##' cannot appear at either end of a macro expansion");
  for (Token *t = body; t->kind != TK_EOF; t = t->next)
    if (tok_equal(t, "##") && t->next->kind == TK_EOF)
      error_tok(t, "'##' cannot appear at either end of a macro expansion");

  Macro *old = hashmap_get(&macros, name);
  if (old) {
    if (old->handler || !old->def) {
      warn_tok(W_MACRO_REDEFINED, name_tok, "redefining builtin macro '%s'", name);
    } else if (old->is_objlike != is_objlike || !same_body(old->body, body) ||
               !same_params(old->params, params)) {
      warn_tok(W_MACRO_REDEFINED, name_tok, "'%s' macro redefined", name);
      if (diag_warning_enabled(W_MACRO_REDEFINED) && !name_tok->file->is_system)
        note_tok(old->def, "previous definition is here");
    }
  }

  Macro *m = add_macro(name, is_objlike, body);
  m->params = params;
  m->va_args_name = va_args_name;
  m->def = name_tok;
  return tok;
}

// ---------------------------------------------------------------------------
// Expansion
// ---------------------------------------------------------------------------

static MacroArg *read_one_arg(Token **rest, Token *tok, bool read_rest) {
  Token head = {};
  Token *cur = &head;
  int depth = 0;
  Token *start = tok;
  for (;;) {
    if (tok->kind == TK_EOF)
      error_tok(start, "unterminated argument list invoking macro");
    if (depth == 0 && tok_equal(tok, ")"))
      break;
    if (depth == 0 && !read_rest && tok_equal(tok, ","))
      break;
    if (tok_equal(tok, "("))
      depth++;
    else if (tok_equal(tok, ")"))
      depth--;
    cur = cur->next = tok_copy(tok);
    tok = tok->next;
  }
  cur->next = tok_new_eof(tok);
  MacroArg *arg = NEW(MacroArg);
  arg->tok = head.next;
  *rest = tok;
  return arg;
}

static int count_params(const MacroParam *p) {
  int n = 0;
  for (; p; p = p->next)
    n++;
  return n;
}

// tok points at the macro name; the next token is "(".
static MacroArg *read_macro_args(Token **rest, Token *tok, Macro *m) {
  Token *name = tok;
  tok = tok->next->next;
  MacroArg head = {};
  MacroArg *cur = &head;

  int nparams = count_params(m->params);
  int given = 0;
  MacroParam *pp = m->params;
  for (; pp; pp = pp->next) {
    if (cur != &head) {
      if (!tok_equal(tok, ","))
        error_tok(name, "macro '%s' requires %d argument%s, but only %d given", m->name, nparams,
                  nparams == 1 ? "" : "s", given);
      tok = tok->next;
    }
    cur = cur->next = read_one_arg(&tok, tok, false);
    cur->name = pp->name;
    given++;
  }

  if (m->va_args_name) {
    MacroArg *arg;
    if (tok_equal(tok, ")")) {
      arg = NEW(MacroArg);
      arg->tok = tok_new_eof(tok);
    } else {
      if (nparams > 0)
        tok = tok_skip(tok, ",");
      arg = read_one_arg(&tok, tok, true);
    }
    arg->name = m->va_args_name;
    arg->is_va_args = true;
    cur = cur->next = arg;
  } else if (!tok_equal(tok, ")")) {
    error_tok(name, "macro '%s' passed too many arguments (expected %d)", m->name, nparams);
  }

  *rest = tok; // the closing parenthesis
  return head.next;
}

static MacroArg *find_arg(MacroArg *args, const Token *tok) {
  for (MacroArg *a = args; a; a = a->next)
    if ((int)strlen(a->name) == tok->len && strncmp(a->name, tok->loc, (size_t)tok->len) == 0)
      return a;
  return nullptr;
}

static Token *expanded_arg(MacroArg *arg) {
  if (!arg->expanded)
    arg->expanded = pp_expand(arg->tok);
  return arg->expanded;
}

// The # operator: backslashes and quotes are escaped only inside string and
// character literals (C23 6.10.5.2).
static Token *stringize(const Token *hash, const Token *arg) {
  StrBuf sb = {};
  sb_putc(&sb, '"');
  for (const Token *t = arg; t->kind != TK_EOF; t = t->next) {
    if (t != arg && t->has_space)
      sb_putc(&sb, ' ');
    bool is_literal = t->kind == TK_STR || (t->kind == TK_NUM && memchr(t->loc, '\'', (size_t)t->len));
    for (int i = 0; i < t->len; i++) {
      if (is_literal && (t->loc[i] == '"' || t->loc[i] == '\\'))
        sb_putc(&sb, '\\');
      sb_putc(&sb, t->loc[i]);
    }
  }
  sb_putc(&sb, '"');
  return pp_tokenize_text(sb.data, hash);
}

// A placemarker stands for an empty argument next to ## (C23 6.10.5.3). It
// is represented by a zero-length punctuator and removed after substitution.
static Token *new_placemarker(const Token *tmpl) {
  Token *t = tok_copy(tmpl);
  t->kind = TK_PUNCT;
  t->len = 0;
  return t;
}

static bool is_placemarker(const Token *t) { return t->kind == TK_PUNCT && t->len == 0; }

static Token *remove_placemarkers(Token *tok) {
  Token head = {.next = tok};
  for (Token *t = &head; t->next;) {
    if (is_placemarker(t->next))
      t->next = t->next->next;
    else
      t = t->next;
  }
  return head.next;
}

// Concatenates two tokens (the ## operator).
static Token *paste(const Token *lhs, const Token *rhs, Token *origin) {
  if (is_placemarker(rhs))
    return tok_copy(lhs);
  if (is_placemarker(lhs)) {
    Token *t = tok_copy(rhs);
    t->has_space = lhs->has_space;
    t->origin = t->origin ? t->origin : origin;
    return t;
  }
  char *buf = format("%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);
  Token *tok = pp_tokenize_text(buf, lhs);
  if (tok->next->kind != TK_EOF)
    error_tok(lhs, "pasting '%.*s' and '%.*s' does not give a valid preprocessing token", lhs->len,
              lhs->loc, rhs->len, rhs->loc);
  tok->has_space = lhs->has_space;
  tok->origin = lhs->origin ? lhs->origin : origin;
  tok->hideset = lhs->hideset;
  return tok;
}

// C23: __VA_OPT__ checks whether the variable arguments expand to nothing.
static bool has_varargs(MacroArg *args) {
  for (MacroArg *a = args; a; a = a->next)
    if (a->is_va_args)
      return expanded_arg(a)->kind != TK_EOF;
  return false;
}

static Token *subst(Token *tok, MacroArg *args, Token *origin, bool funclike);

static Token *copy_body_token(const Token *tok, Token *origin) {
  Token *t = tok_copy(tok);
  t->origin = origin;
  return t;
}

// Parses __VA_OPT__( content ) and returns its substituted replacement.
static Token *va_opt(Token **rest, Token *tok, MacroArg *args, Token *origin) {
  Token *start = tok;
  tok = tok_skip(tok->next, "(");
  Token head = {};
  Token *cur = &head;
  int depth = 0;
  for (;; tok = tok->next) {
    if (tok->kind == TK_EOF)
      error_tok(start, "unterminated __VA_OPT__");
    if (tok_equal(tok, "(")) {
      depth++;
    } else if (tok_equal(tok, ")")) {
      if (depth-- == 0)
        break;
    }
    cur = cur->next = tok_copy(tok);
  }
  cur->next = tok_new_eof(tok);
  *rest = tok->next;
  if (!has_varargs(args))
    return tok_new_eof(tok);
  return subst(head.next, args, origin, true);
}

// Appends an argument unexpanded (operand of ##); empty becomes a placemarker.
static Token *append_raw_arg(Token *cur, MacroArg *arg, const Token *at) {
  if (arg->tok->kind == TK_EOF)
    return cur->next = new_placemarker(at);
  for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next) {
    cur = cur->next = tok_copy(t);
    if (t == arg->tok)
      cur->has_space = at->has_space;
  }
  return cur;
}

// Replaces parameters in a macro body with arguments and performs # and ##.
static Token *subst(Token *tok, MacroArg *args, Token *origin, bool funclike) {
  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // #param -> string literal
    if (funclike && tok_equal(tok, "#")) {
      Token *hash = tok;
      Token *content;
      if (tok_equal(tok->next, "__VA_OPT__")) {
        content = va_opt(&tok, tok->next, args, origin);
      } else {
        MacroArg *arg = find_arg(args, tok->next);
        if (!arg)
          error_tok(tok, "'#' is not followed by a macro parameter");
        content = arg->tok;
        tok = tok->next->next;
      }
      cur = cur->next = stringize(hash, remove_placemarkers(content));
      cur->has_space = hash->has_space;
      cur->origin = origin;
      continue;
    }

    // GNU: ", ## __VA_ARGS__" drops the comma when there are no varargs.
    if (funclike && tok_equal(tok, ",") && tok_equal(tok->next, "##")) {
      MacroArg *arg = find_arg(args, tok->next->next);
      if (arg && arg->is_va_args) {
        if (arg->tok->kind == TK_EOF) {
          tok = tok->next->next->next;
        } else {
          cur = cur->next = copy_body_token(tok, origin);
          tok = tok->next->next;
        }
        continue;
      }
    }

    if (funclike && tok_equal(tok, "__VA_OPT__") && tok_equal(tok->next, "(")) {
      Token *start = tok;
      Token *content = va_opt(&tok, tok, args, origin);
      if (content->kind == TK_EOF) {
        cur = cur->next = new_placemarker(start);
      } else {
        for (Token *t = content; t->kind != TK_EOF; t = t->next) {
          cur = cur->next = tok_copy(t);
          if (t == content)
            cur->has_space = start->has_space;
        }
      }
      continue;
    }

    // lhs ## rhs (lhs is already in the output list)
    if (tok_equal(tok, "##")) {
      Token *rhs = tok->next;
      MacroArg *arg = find_arg(args, rhs);
      if (arg) {
        Token *t = arg->tok;
        if (t->kind != TK_EOF) {
          *cur = *paste(cur, t, origin);
          for (t = t->next; t->kind != TK_EOF; t = t->next)
            cur = cur->next = tok_copy(t);
        }
      } else {
        *cur = *paste(cur, rhs, origin);
      }
      tok = rhs->next;
      continue;
    }

    MacroArg *arg = find_arg(args, tok);

    // param ## ...: the argument is used unexpanded.
    if (arg && tok_equal(tok->next, "##")) {
      cur = append_raw_arg(cur, arg, tok);
      tok = tok->next;
      continue;
    }

    // Plain parameter: substitute the fully expanded argument.
    if (arg) {
      for (Token *t = expanded_arg(arg); t->kind != TK_EOF; t = t->next) {
        cur = cur->next = tok_copy(t);
        if (t == arg->expanded)
          cur->has_space = tok->has_space;
      }
      tok = tok->next;
      continue;
    }

    cur = cur->next = copy_body_token(tok, origin);
    tok = tok->next;
  }

  cur->next = tok;
  return head.next;
}

bool macro_try_expand(Token **rest, Token *tok) {
  if (hideset_contains(tok->hideset, tok->loc, tok->len))
    return false;
  Macro *m = find_macro(tok);
  if (!m)
    return false;

  if (m->handler) {
    Token *t = m->handler(tok);
    t->at_bol = tok->at_bol;
    t->has_space = tok->has_space;
    t->next = tok->next;
    t->origin = tok;
    *rest = t;
    pp_stats.expansions++;
    return true;
  }

  if (m->is_objlike) {
    Hideset *hs = hideset_union(tok->hideset, new_hideset(m->name));
    Token *body = add_hideset(remove_placemarkers(subst(m->body, nullptr, tok, false)), hs);
    *rest = pp_append(body, tok->next);
    if (body->kind != TK_EOF) {
      (*rest)->at_bol = tok->at_bol;
      (*rest)->has_space = tok->has_space;
    }
    pp_stats.expansions++;
    return true;
  }

  // A function-like macro name not followed by "(" is an ordinary identifier.
  if (!tok_equal(tok->next, "("))
    return false;

  Token *name = tok;
  MacroArg *args = read_macro_args(&tok, tok, m);
  Token *rparen = tok;

  Hideset *hs = hideset_intersection(name->hideset, rparen->hideset);
  hs = hideset_union(hs, new_hideset(m->name));
  Token *body = add_hideset(remove_placemarkers(subst(m->body, args, name, true)), hs);
  *rest = pp_append(body, rparen->next);
  if (body->kind != TK_EOF) {
    (*rest)->at_bol = name->at_bol;
    (*rest)->has_space = name->has_space;
  }
  pp_stats.expansions++;
  return true;
}

Token *pp_expand(Token *tok) {
  Token head = {};
  Token *cur = &head;
  while (tok->kind != TK_EOF) {
    if (macro_try_expand(&tok, tok))
      continue;
    cur = cur->next = tok_copy(tok);
    tok = tok->next;
  }
  cur->next = tok;
  return head.next;
}

// ---------------------------------------------------------------------------
// Builtin macros
// ---------------------------------------------------------------------------

static const Token *root_token(const Token *tok) {
  while (tok->origin)
    tok = tok->origin;
  return tok;
}

static char *(*base_file_hook)(void);

void macro_set_file_hooks(char *(*base_file)(void)) { base_file_hook = base_file; }

static Token *file_macro(Token *tok) {
  const Token *root = root_token(tok);
  return pp_new_str_token(root->file->display_name, root);
}

static Token *line_macro(Token *tok) { return new_num_token(root_token(tok)->line, tok); }

static Token *counter_macro(Token *tok) { return new_num_token(counter_value++, tok); }

static Token *base_file_macro(Token *tok) {
  return pp_new_str_token(base_file_hook ? base_file_hook() : "", tok);
}

static struct tm *build_time(void) {
  static struct tm tm;
  static bool init;
  if (!init) {
    time_t now = time(nullptr);
    localtime_r(&now, &tm);
    init = true;
  }
  return &tm;
}

static Token *date_macro(Token *tok) {
  static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  struct tm *tm = build_time();
  return pp_tokenize_text(format("\"%s %2d %d\"", mon[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900), tok);
}

static Token *time_macro(Token *tok) {
  struct tm *tm = build_time();
  return pp_tokenize_text(format("\"%02d:%02d:%02d\"", tm->tm_hour, tm->tm_min, tm->tm_sec), tok);
}

// Operators that are only meaningful inside #if; they are registered as
// macros so that "#ifdef __has_include" works as in GCC and Clang.
static Token *if_only_operator(Token *tok) {
  error_tok(tok, "'%.*s' may only be used in #if and #elif", tok->len, tok->loc);
}

static void add_builtin(char *name, MacroHandler *fn) {
  Macro *m = add_macro(name, true, nullptr);
  m->handler = fn;
}

// Target and language description, in the form of ordinary #defines.
static const char predefined_source[] =
    "#define __STDC__ 1\n"
    "#define __STDC_VERSION__ 202311L\n"
    "#define __STDC_HOSTED__ 1\n"
    "#define __STDC_UTF_16__ 1\n"
    "#define __STDC_UTF_32__ 1\n"
    "#define __STDC_IEC_559__ 1\n"
    "#define __STDC_NO_ATOMICS__ 1\n"
    "#define __STDC_NO_COMPLEX__ 1\n"
    "#define __STDC_NO_THREADS__ 1\n"
    "#define __STDC_NO_VLA__ 1\n"
    "#define __STDC_EMBED_NOT_FOUND__ 0\n"
    "#define __STDC_EMBED_FOUND__ 1\n"
    "#define __STDC_EMBED_EMPTY__ 2\n"
    "#define __occ__ 1\n"
    "#define __OCC__ 1\n"
    "#define __occ_version__ \"" OCC_VERSION "\"\n"
    "#define __x86_64__ 1\n"
    "#define __x86_64 1\n"
    "#define __amd64__ 1\n"
    "#define __amd64 1\n"
    "#define __linux__ 1\n"
    "#define __linux 1\n"
    "#define __gnu_linux__ 1\n"
    "#define __unix__ 1\n"
    "#define __unix 1\n"
    "#define __ELF__ 1\n"
    "#define __LP64__ 1\n"
    "#define _LP64 1\n"
    "#define __CHAR_BIT__ 8\n"
    "#define __BYTE_ORDER__ 1234\n"
    "#define __ORDER_LITTLE_ENDIAN__ 1234\n"
    "#define __ORDER_BIG_ENDIAN__ 4321\n"
    "#define __SIZEOF_SHORT__ 2\n"
    "#define __SIZEOF_INT__ 4\n"
    "#define __SIZEOF_LONG__ 8\n"
    "#define __SIZEOF_LONG_LONG__ 8\n"
    "#define __SIZEOF_POINTER__ 8\n"
    "#define __SIZEOF_FLOAT__ 4\n"
    "#define __SIZEOF_DOUBLE__ 8\n"
    "#define __SIZEOF_LONG_DOUBLE__ 8\n"
    "#define __SIZEOF_SIZE_T__ 8\n"
    "#define __SIZEOF_WCHAR_T__ 4\n"
    "#define __SIZEOF_WINT_T__ 4\n"
    "#define __SIZEOF_PTRDIFF_T__ 8\n"
    "#define __SIZE_TYPE__ unsigned long\n"
    "#define __PTRDIFF_TYPE__ long\n"
    "#define __WCHAR_TYPE__ int\n"
    "#define __WINT_TYPE__ unsigned int\n"
    "#define __INTMAX_TYPE__ long\n"
    "#define __UINTMAX_TYPE__ unsigned long\n"
    "#define __INTPTR_TYPE__ long\n"
    "#define __UINTPTR_TYPE__ unsigned long\n"
    "#define __CHAR8_TYPE__ unsigned char\n"
    "#define __CHAR16_TYPE__ unsigned short\n"
    "#define __CHAR32_TYPE__ unsigned int\n"
    "#define __SCHAR_MAX__ 127\n"
    "#define __SHRT_MAX__ 32767\n"
    "#define __INT_MAX__ 2147483647\n"
    "#define __LONG_MAX__ 9223372036854775807L\n"
    "#define __LONG_LONG_MAX__ 9223372036854775807LL\n"
    "#define __WCHAR_MAX__ 2147483647\n"
    "#define __WCHAR_MIN__ (-__WCHAR_MAX__ - 1)\n"
    "#define __SIZE_MAX__ 18446744073709551615UL\n"
    "#define __PTRDIFF_MAX__ 9223372036854775807L\n"
    "#define __INTMAX_MAX__ 9223372036854775807L\n"
    "#define __UINTMAX_MAX__ 18446744073709551615UL\n"
    "#define __INTPTR_MAX__ 9223372036854775807L\n"
    "#define __UINTPTR_MAX__ 18446744073709551615UL\n";

static void define_from_source(const char *src, const char *name) {
  SourceFile *file = source_new(name, xstrdup(src));
  file->is_system = true;
  Token *tok = tokenize(file);
  while (tok->kind != TK_EOF) {
    // Every line has the shape: # define NAME BODY
    tok = macro_define(tok->next->next);
    // macro_define records the definition token; clear it so that user
    // redefinitions are reported as "redefining builtin macro".
  }
}

void macro_reset(const PreprocOptions *opts) {
  macros = (HashMap){};
  counter_value = 0;

  define_from_source(predefined_source, "<built-in>");
  // Predefined macros count as builtins for redefinition warnings.
  for (int i = 0; i < macros.capacity; i++) {
    HashEntry *e = &macros.buckets[i];
    if (e->key && e->val)
      ((Macro *)e->val)->def = nullptr;
  }

  add_builtin("__FILE__", file_macro);
  add_builtin("__LINE__", line_macro);
  add_builtin("__COUNTER__", counter_macro);
  add_builtin("__DATE__", date_macro);
  add_builtin("__TIME__", time_macro);
  add_builtin("__BASE_FILE__", base_file_macro);
  add_builtin("__has_include", if_only_operator);
  add_builtin("__has_include_next", if_only_operator);
  add_builtin("__has_embed", if_only_operator);
  add_builtin("__has_c_attribute", if_only_operator);

  StrBuf src = {};
  if (opts->optimize)
    sb_puts(&src, "#define __OPTIMIZE__ 1\n");
  for (size_t i = 0; i < opts->defines.len; i++) {
    char *def = opts->defines.data[i];
    char *eq = strchr(def, '=');
    if (eq)
      sb_printf(&src, "#define %.*s %s\n", (int)(eq - def), def, eq + 1);
    else
      sb_printf(&src, "#define %s 1\n", def);
  }
  if (src.len)
    define_from_source(src.data, "<command line>");

  for (size_t i = 0; i < opts->undefines.len; i++)
    hashmap_delete(&macros, opts->undefines.data[i]);
}

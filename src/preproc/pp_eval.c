// pp_eval.c - evaluation of #if / #elif controlling expressions.
//
// Arithmetic is done in intmax_t / uintmax_t as C23 6.10.2 requires; the
// usual arithmetic conversions pick unsigned when either operand is unsigned.
#include "preproc/pp_internal.h"

typedef struct {
  int64_t v;
  bool is_unsigned;
} PPValue;

typedef struct {
  Token *tok;
  const Token *directive;
  bool evaluate; // false inside the unevaluated arm of && || ?:
} PPParser;

static PPValue cond_expr(PPParser *p);

static int64_t has_c_attribute(const Token *name) {
  static const struct {
    const char *name;
    int64_t version;
  } attrs[] = {
      {"deprecated", 201904}, {"fallthrough", 201904}, {"maybe_unused", 201904},
      {"nodiscard", 202003},  {"noreturn", 202202},    {"_Noreturn", 202202},
      {"unsequenced", 202207}, {"reproducible", 202207},
  };
  for (size_t i = 0; i < ARRAY_LEN(attrs); i++)
    if (tok_equal(name, attrs[i].name))
      return attrs[i].version;
  return 0;
}

static Token *new_value_token(int64_t val, const Token *tmpl) {
  Token *t = pp_tokenize_text(format("%lld", (long long)val), tmpl);
  t->has_space = tmpl->has_space;
  return t;
}

// Replaces defined(X), __has_include(...) and friends by 0/1 tokens. These
// operands must not be macro-expanded, so this runs before expansion.
static Token *replace_operators(Token *tok) {
  Token head = {};
  Token *cur = &head;
  while (tok->kind != TK_EOF) {
    if (tok_equal(tok, "defined")) {
      Token *start = tok;
      bool has_paren = tok_consume(&tok, tok->next, "(");
      if (tok->kind != TK_IDENT)
        error_tok(start, "macro name must be an identifier");
      Token *name = tok;
      tok = tok->next;
      if (has_paren)
        tok = tok_skip(tok, ")");
      cur = cur->next = new_value_token(macro_is_defined(name), start);
      continue;
    }
    if (tok_equal(tok, "__has_include") || tok_equal(tok, "__has_include_next")) {
      Token *start = tok;
      bool found = pp_has_include(tok->next, &tok);
      cur = cur->next = new_value_token(found, start);
      continue;
    }
    if (tok_equal(tok, "__has_embed")) {
      Token *start = tok;
      int status;
      pp_has_embed_resource(tok->next, &tok, &status);
      cur = cur->next = new_value_token(status, start);
      continue;
    }
    if (tok_equal(tok, "__has_c_attribute")) {
      Token *start = tok;
      tok = tok_skip(tok->next, "(");
      Token *name = tok;
      // Accept vendor-prefixed names like gnu::packed (not supported: 0).
      if (tok_equal(tok->next, "::")) {
        tok = tok->next->next->next;
        cur = cur->next = new_value_token(0, start);
      } else {
        tok = tok->next;
        cur = cur->next = new_value_token(has_c_attribute(name), start);
      }
      tok = tok_skip(tok, ")");
      continue;
    }
    cur = cur->next = tok_copy(tok);
    tok = tok->next;
  }
  cur->next = tok;
  return head.next;
}

static PPValue value(int64_t v, bool is_unsigned) { return (PPValue){v, is_unsigned}; }

static PPValue primary(PPParser *p) {
  Token *tok = p->tok;
  if (tok_equal(tok, "(")) {
    p->tok = tok->next;
    PPValue v = cond_expr(p);
    p->tok = tok_skip(p->tok, ")");
    return v;
  }
  if (tok->kind == TK_PP_NUM) {
    if (!convert_pp_int(tok))
      error_tok(tok, "invalid integer constant in preprocessor expression");
    p->tok = tok->next;
    bool u = tok->lit == LIT_UINT || tok->lit == LIT_ULONG || tok->lit == LIT_ULLONG;
    return value(tok->ival, u);
  }
  if (tok->kind == TK_NUM) { // character constant
    p->tok = tok->next;
    return value(tok->ival, tok->lit != LIT_INT);
  }
  if (tok->kind == TK_IDENT) {
    // After expansion, remaining identifiers are 0, except true/false (C23).
    p->tok = tok->next;
    if (tok_equal(tok, "true"))
      return value(1, false);
    if (tok->kind == TK_IDENT && tok_equal(tok->next, "("))
      error_tok(tok, "function-like macro '%.*s' is not defined", tok->len, tok->loc);
    return value(0, false);
  }
  if (tok->kind == TK_EOF)
    error_tok(p->directive, "expected value in expression");
  error_tok(tok, "token is not a valid binary operator in a preprocessor subexpression");
}

static PPValue unary(PPParser *p) {
  Token *tok = p->tok;
  if (tok_equal(tok, "+")) {
    p->tok = tok->next;
    return unary(p);
  }
  if (tok_equal(tok, "-")) {
    p->tok = tok->next;
    PPValue v = unary(p);
    return value((int64_t)(0 - (uint64_t)v.v), v.is_unsigned);
  }
  if (tok_equal(tok, "~")) {
    p->tok = tok->next;
    PPValue v = unary(p);
    return value(~v.v, v.is_unsigned);
  }
  if (tok_equal(tok, "!")) {
    p->tok = tok->next;
    PPValue v = unary(p);
    return value(!v.v, false);
  }
  return primary(p);
}

// Binary operator precedence (higher binds tighter); 0 = not a binary operator.
static int precedence(const Token *tok) {
  static const struct {
    const char *op;
    int prec;
  } table[] = {
      {"*", 10}, {"/", 10}, {"%", 10}, {"+", 9},  {"-", 9},  {"<<", 8}, {">>", 8},
      {"<", 7},  {"<=", 7}, {">", 7},  {">=", 7}, {"==", 6}, {"!=", 6}, {"&", 5},
      {"^", 4},  {"|", 3},  {"&&", 2}, {"||", 1},
  };
  if (tok->kind != TK_PUNCT)
    return 0;
  for (size_t i = 0; i < ARRAY_LEN(table); i++)
    if (tok_equal(tok, table[i].op))
      return table[i].prec;
  return 0;
}

static PPValue apply(PPParser *p, const Token *op, PPValue a, PPValue b) {
  bool u = a.is_unsigned || b.is_unsigned;
  uint64_t ua = (uint64_t)a.v, ub = (uint64_t)b.v;
  if (tok_equal(op, "*"))
    return value((int64_t)(ua * ub), u);
  if (tok_equal(op, "/") || tok_equal(op, "%")) {
    if (b.v == 0) {
      if (p->evaluate)
        error_tok(op, "division by zero in preprocessor expression");
      return value(0, u);
    }
    bool div = tok_equal(op, "/");
    if (u)
      return value((int64_t)(div ? ua / ub : ua % ub), true);
    if (a.v == INT64_MIN && b.v == -1)
      return value(div ? INT64_MIN : 0, false);
    return value(div ? a.v / b.v : a.v % b.v, false);
  }
  if (tok_equal(op, "+"))
    return value((int64_t)(ua + ub), u);
  if (tok_equal(op, "-"))
    return value((int64_t)(ua - ub), u);
  if (tok_equal(op, "<<"))
    return value(ub >= 64 ? 0 : (int64_t)(ua << ub), a.is_unsigned);
  if (tok_equal(op, ">>")) {
    if (ub >= 64)
      return value(a.is_unsigned || a.v >= 0 ? 0 : -1, a.is_unsigned);
    return value(a.is_unsigned ? (int64_t)(ua >> ub) : a.v >> ub, a.is_unsigned);
  }
  if (tok_equal(op, "<"))
    return value(u ? ua < ub : a.v < b.v, false);
  if (tok_equal(op, "<="))
    return value(u ? ua <= ub : a.v <= b.v, false);
  if (tok_equal(op, ">"))
    return value(u ? ua > ub : a.v > b.v, false);
  if (tok_equal(op, ">="))
    return value(u ? ua >= ub : a.v >= b.v, false);
  if (tok_equal(op, "=="))
    return value(a.v == b.v, false);
  if (tok_equal(op, "!="))
    return value(a.v != b.v, false);
  if (tok_equal(op, "&"))
    return value(a.v & b.v, u);
  if (tok_equal(op, "^"))
    return value(a.v ^ b.v, u);
  if (tok_equal(op, "|"))
    return value(a.v | b.v, u);
  ICE_UNREACHABLE();
}

// Precedence climbing over binary operators.
static PPValue binary(PPParser *p, int min_prec) {
  PPValue lhs = unary(p);
  for (;;) {
    Token *op = p->tok;
    int prec = precedence(op);
    if (prec == 0 || prec < min_prec)
      return lhs;
    p->tok = op->next;

    if (tok_equal(op, "&&") || tok_equal(op, "||")) {
      bool is_and = tok_equal(op, "&&");
      bool saved = p->evaluate;
      bool short_circuit = is_and ? !lhs.v : lhs.v != 0;
      if (short_circuit)
        p->evaluate = false;
      PPValue rhs = binary(p, prec + 1);
      p->evaluate = saved;
      lhs = value(is_and ? (lhs.v && rhs.v) : (lhs.v || rhs.v), false);
      continue;
    }
    PPValue rhs = binary(p, prec + 1);
    lhs = apply(p, op, lhs, rhs);
  }
}

static PPValue cond_expr(PPParser *p) {
  PPValue c = binary(p, 1);
  if (!tok_equal(p->tok, "?"))
    return c;
  p->tok = p->tok->next;
  bool saved = p->evaluate;
  p->evaluate = saved && c.v;
  PPValue a = cond_expr(p);
  p->tok = tok_skip(p->tok, ":");
  p->evaluate = saved && !c.v;
  PPValue b = cond_expr(p);
  p->evaluate = saved;
  PPValue r = c.v ? a : b;
  r.is_unsigned = a.is_unsigned || b.is_unsigned;
  return r;
}

bool pp_eval_condition(Token *line, const Token *directive) {
  if (line->kind == TK_EOF)
    error_tok(directive, "#%.*s with no expression", directive->len, directive->loc);
  Token *tok = replace_operators(line);
  tok = pp_expand(tok);
  // "defined" produced by macro expansion is undefined behavior in ISO C, but
  // it is common in real headers; evaluate it like GCC does.
  tok = replace_operators(tok);

  PPParser p = {.tok = tok, .directive = directive, .evaluate = true};
  PPValue v = cond_expr(&p);
  while (p.tok->kind != TK_EOF && tok_equal(p.tok, ","))
    error_tok(p.tok, "comma operator in operand of #if");
  if (p.tok->kind != TK_EOF)
    error_tok(p.tok, "extra tokens in preprocessor expression");
  return v.v != 0;
}

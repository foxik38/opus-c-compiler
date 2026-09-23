// fold.c - AST optimizations for -o prod.
//
//   * constant folding:        2 * 8 + 1       -> 17
//   * algebraic identities:    x * 1, x + 0    -> x
//   * strength reduction:      x * 8           -> x << 3
//                              u / 16, u % 16  -> u >> 4, u & 15  (unsigned)
//   * dead branches:           if (0) ...      -> removed
//   * dead expression statements without side effects are dropped
//
// Everything here preserves the evaluation of side effects; an operand is
// only discarded if evaluating it has no observable effect.
#include "opt/opt.h"
#include "parse/parse.h"

static OptStats *stats;

static Node *opt_expr(Node *n);
static Node *opt_stmt(Node *n);

static Node *make_num(Node *like, int64_t val) {
  Node *n = NEW(Node);
  n->kind = ND_NUM;
  n->tok = like->tok;
  n->ty = like->ty;
  n->val = val;
  return n;
}

static Node *make_fnum(Node *like, double val) {
  Node *n = make_num(like, 0);
  n->fval = val;
  return n;
}

static bool is_leaf_const(const Node *n) {
  return n && (n->kind == ND_NUM || (n->kind == ND_VAR && n->var->has_const_value && is_scalar(n->ty)));
}

static bool is_int_const(const Node *n, int64_t v) {
  return n->kind == ND_NUM && is_integer(n->ty) && n->val == v;
}

static bool is_foldable_op(NodeKind k) {
  switch (k) {
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD: case ND_BITAND: case ND_BITOR:
  case ND_BITXOR: case ND_SHL: case ND_SHR: case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_NEG:
  case ND_NOT: case ND_BITNOT: case ND_CAST: case ND_LOGAND: case ND_LOGOR:
    return true;
  default:
    return false;
  }
}

// Evaluates an operator whose operands are all constants.
static Node *try_fold(Node *n) {
  if (!is_foldable_op(n->kind) || !is_scalar(n->ty))
    return n;
  if (!is_leaf_const(n->lhs) || (n->rhs && !is_leaf_const(n->rhs)))
    return n;
  if (!is_const_expr(n) || (!is_flonum(n->ty) && !is_const_int_expr(n)))
    return n; // e.g. division by zero, or an address constant
  stats->folded++;
  if (is_flonum(n->ty))
    return make_fnum(n, eval_double(n));
  return make_num(n, eval_int(n));
}

// !!x as an int: the truth value of a scalar.
static Node *truth_value(Node *x) {
  Node *inner = NEW(Node);
  inner->kind = ND_NOT;
  inner->tok = x->tok;
  inner->lhs = x;
  inner->ty = ty_int;
  Node *outer = NEW(Node);
  outer->kind = ND_NOT;
  outer->tok = x->tok;
  outer->lhs = inner;
  outer->ty = ty_int;
  return outer;
}

static Node *make_comma(Node *lhs, Node *rhs) {
  Node *n = NEW(Node);
  n->kind = ND_COMMA;
  n->tok = rhs->tok;
  n->lhs = lhs;
  n->rhs = rhs;
  n->ty = rhs->ty;
  return n;
}

static Node *simplified(Node *n) {
  stats->simplified++;
  return n;
}

// Algebraic identities and strength reduction on integer operators.
static Node *simplify(Node *n) {
  if (!n->lhs || !n->rhs || !is_integer(n->ty) || !is_integer(n->lhs->ty))
    return n;
  Node *l = n->lhs, *r = n->rhs;
  bool l_pure = !has_side_effects(l), r_pure = !has_side_effects(r);

  switch (n->kind) {
  case ND_ADD:
    if (is_int_const(r, 0))
      return simplified(l);
    if (is_int_const(l, 0))
      return simplified(r);
    break;
  case ND_SUB:
    if (is_int_const(r, 0))
      return simplified(l);
    break;
  case ND_MUL:
    if (is_int_const(r, 1))
      return simplified(l);
    if (is_int_const(l, 1))
      return simplified(r);
    if (is_int_const(r, 0) && l_pure)
      return simplified(make_num(n, 0));
    if (r->kind == ND_NUM && r->val > 1 && is_power_of_two((uint64_t)r->val)) {
      n->kind = ND_SHL;
      n->rhs = make_num(r, log2_u64((uint64_t)r->val));
      n->rhs->ty = ty_int;
      return simplified(n);
    }
    break;
  case ND_DIV:
    if (is_int_const(r, 1))
      return simplified(l);
    if (n->ty->is_unsigned && r->kind == ND_NUM && r->val > 1 && is_power_of_two((uint64_t)r->val)) {
      n->kind = ND_SHR;
      n->rhs = make_num(r, log2_u64((uint64_t)r->val));
      n->rhs->ty = ty_int;
      return simplified(n);
    }
    break;
  case ND_MOD:
    if (n->ty->is_unsigned && r->kind == ND_NUM && r->val > 0 && is_power_of_two((uint64_t)r->val)) {
      n->kind = ND_BITAND;
      n->rhs = make_num(r, r->val - 1);
      return simplified(n);
    }
    break;
  case ND_BITAND:
    if (is_int_const(r, 0) && l_pure)
      return simplified(make_num(n, 0));
    if (is_int_const(r, -1))
      return simplified(l);
    break;
  case ND_BITOR:
  case ND_BITXOR:
    if (is_int_const(r, 0))
      return simplified(l);
    if (is_int_const(l, 0))
      return simplified(r);
    break;
  case ND_SHL:
  case ND_SHR:
    if (is_int_const(r, 0))
      return simplified(l);
    break;
  default:
    break;
  }
  (void)r_pure;
  return n;
}

// && and || with a constant operand.
static Node *simplify_logical(Node *n) {
  Node *l = n->lhs, *r = n->rhs;
  bool is_and = n->kind == ND_LOGAND;
  if (l->kind == ND_NUM && is_integer(l->ty)) {
    stats->branches++;
    bool v = l->val != 0;
    if (is_and != v)          // 0 && x, 1 || x
      return make_num(n, v);
    return truth_value(r);    // 1 && x, 0 || x
  }
  if (r->kind == ND_NUM && is_integer(r->ty)) {
    bool v = r->val != 0;
    stats->branches++;
    if (is_and == v)          // x && 1, x || 0
      return truth_value(l);
    Node *k = make_num(n, v); // x && 0, x || 1
    return has_side_effects(l) ? make_comma(l, k) : k;
  }
  return n;
}

static void opt_args(Node *n) {
  Node head = {.next = n->args};
  for (Node *p = &head; p->next; p = p->next) {
    Node *next = p->next->next;
    p->next = opt_expr(p->next);
    p->next->next = next;
  }
  n->args = head.next;
}

static Node *opt_expr(Node *n) {
  if (!n)
    return n;

  // Right-leaning comma chains (initializers) are walked iteratively.
  if (n->kind == ND_COMMA) {
    for (Node *c = n; c->kind == ND_COMMA; c = c->rhs) {
      c->lhs = opt_expr(c->lhs);
      if (c->rhs->kind != ND_COMMA)
        c->rhs = opt_expr(c->rhs);
    }
    if (!has_side_effects(n->lhs) && n->rhs->kind != ND_COMMA && !n->is_compound_literal) {
      stats->simplified++;
      return n->rhs;
    }
    return n;
  }

  n->lhs = opt_expr(n->lhs);
  n->rhs = opt_expr(n->rhs);
  n->cond = opt_expr(n->cond);
  n->then = opt_expr(n->then);
  n->els = opt_expr(n->els);
  if (n->kind == ND_FUNCALL)
    opt_args(n);

  switch (n->kind) {
  case ND_COND:
    if (n->cond->kind == ND_NUM && is_integer(n->cond->ty)) {
      stats->branches++;
      return n->cond->val ? n->then : n->els;
    }
    return n;
  case ND_LOGAND:
  case ND_LOGOR: {
    Node *f = try_fold(n);
    return f != n ? f : simplify_logical(n);
  }
  default: {
    Node *f = try_fold(n);
    return f != n ? f : simplify(n);
  }
  }
}

static bool contains_label(Node *n) {
  if (!n)
    return false;
  if (n->kind == ND_LABEL || n->kind == ND_CASE)
    return true;
  if (n->kind == ND_BLOCK)
    for (Node *s = n->body; s; s = s->next)
      if (contains_label(s))
        return true;
  return contains_label(n->then) || contains_label(n->els) || contains_label(n->init) ||
         (n->kind != ND_EXPR_STMT && n->kind != ND_RETURN && contains_label(n->lhs));
}

static Node *empty_stmt(Node *like) {
  Node *n = NEW(Node);
  n->kind = ND_BLOCK;
  n->tok = like->tok;
  return n;
}

static bool is_const_cond(Node *c, bool *value) {
  if (!c || c->kind != ND_NUM || !is_integer(c->ty))
    return false;
  *value = c->val != 0;
  return true;
}

static Node *opt_stmt(Node *n) {
  if (!n)
    return n;
  bool v;
  switch (n->kind) {
  case ND_BLOCK: {
    Node head = {.next = n->body};
    for (Node *p = &head; p->next; p = p->next) {
      Node *next = p->next->next;
      p->next = opt_stmt(p->next);
      p->next->next = next;
    }
    n->body = head.next;
    return n;
  }
  case ND_EXPR_STMT:
    n->lhs = opt_expr(n->lhs);
    if (!has_side_effects(n->lhs)) {
      stats->simplified++;
      return empty_stmt(n);
    }
    return n;
  case ND_RETURN:
    n->lhs = opt_expr(n->lhs);
    return n;
  case ND_IF:
    n->cond = opt_expr(n->cond);
    n->then = opt_stmt(n->then);
    n->els = opt_stmt(n->els);
    if (is_const_cond(n->cond, &v)) {
      Node *dead = v ? n->els : n->then;
      if (!contains_label(dead)) {
        stats->branches++;
        Node *live = v ? n->then : n->els;
        return live ? live : empty_stmt(n);
      }
    }
    return n;
  case ND_FOR:
    n->init = opt_stmt(n->init);
    n->cond = opt_expr(n->cond);
    n->inc = opt_expr(n->inc);
    n->then = opt_stmt(n->then);
    if (is_const_cond(n->cond, &v)) {
      if (v) {
        n->cond = nullptr; // for (;;)
      } else if (!contains_label(n->then)) {
        stats->branches++;
        return n->init ? n->init : empty_stmt(n); // while (0) body is dead
      }
    }
    return n;
  case ND_DO:
    n->then = opt_stmt(n->then);
    n->cond = opt_expr(n->cond);
    return n;
  case ND_SWITCH:
    n->cond = opt_expr(n->cond);
    n->then = opt_stmt(n->then);
    return n;
  case ND_CASE:
  case ND_LABEL:
    n->lhs = opt_stmt(n->lhs);
    return n;
  default:
    return n;
  }
}

void optimize_program(Program *prog, OptStats *out) {
  stats = out;
  for (Obj *fn = prog->globals; fn; fn = fn->next)
    if (fn->is_function && fn->body)
      fn->body = opt_stmt(fn->body);
}

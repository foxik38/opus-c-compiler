// const_eval.c - evaluation of constant expressions (C23 6.6).
//
// Integer evaluation tracks an optional relocation label so that address
// constants such as "&arr[3]" can initialize objects with static storage.
#include "parse/parser.h"

static int64_t eval_rec(Node *node, char **label, bool *ok);
static double eval_fp(Node *node, bool *ok);

static int64_t fail(bool *ok) {
  *ok = false;
  return 0;
}

// Truncates a value to the width and signedness of `ty`.
static int64_t wrap(int64_t v, const Type *ty) {
  if (ty->kind == TY_BOOL)
    return v != 0;
  if (!is_integer(ty) || ty->size >= 8)
    return v;
  int bits = ty->size * 8;
  uint64_t mask = (1ULL << bits) - 1;
  uint64_t u = (uint64_t)v & mask;
  if (!ty->is_unsigned && (u >> (bits - 1)))
    u |= ~mask;
  return (int64_t)u;
}

static bool is_unsigned_op(const Type *ty) { return ty->is_unsigned || is_pointer_like(ty); }

static bool eval_truth(Node *node, bool *ok) {
  if (is_flonum(node->ty))
    return eval_fp(node, ok) != 0;
  char *label = nullptr;
  int64_t v = eval_rec(node, &label, ok);
  return label || v != 0; // the address of an object is never null
}

static int64_t eval_addr(Node *node, char **label, bool *ok) {
  switch (node->kind) {
  case ND_VAR:
    if (node->var->is_local || !label)
      return fail(ok);
    *label = node->var->name;
    return 0;
  case ND_DEREF:
    return eval_rec(node->lhs, label, ok);
  case ND_MEMBER:
    return eval_addr(node->lhs, label, ok) + node->member->offset;
  default:
    return fail(ok);
  }
}

static int64_t eval_rec(Node *node, char **label, bool *ok) {
  if (!*ok)
    return 0;
  if (is_flonum(node->ty))
    return (int64_t)eval_fp(node, ok);

  Type *ty = node->ty;
  switch (node->kind) {
  case ND_NUM:
    return wrap(node->val, ty);

  case ND_ADD: {
    int64_t l = eval_rec(node->lhs, label, ok);
    return wrap((int64_t)((uint64_t)l + (uint64_t)eval_rec(node->rhs, nullptr, ok)), ty);
  }
  case ND_SUB: {
    if (node->lhs->ty->kind == TY_PTR && node->rhs->ty->kind == TY_PTR) {
      // The distance between two addresses in the same object is constant.
      char *l1 = nullptr, *l2 = nullptr;
      int64_t a = eval_rec(node->lhs, &l1, ok);
      int64_t b = eval_rec(node->rhs, &l2, ok);
      if ((l1 || l2) && (!l1 || !l2 || strcmp(l1, l2) != 0))
        return fail(ok);
      return a - b;
    }
    int64_t l = eval_rec(node->lhs, label, ok);
    return wrap((int64_t)((uint64_t)l - (uint64_t)eval_rec(node->rhs, nullptr, ok)), ty);
  }
  case ND_MUL:
    return wrap((int64_t)((uint64_t)eval_rec(node->lhs, nullptr, ok) * (uint64_t)eval_rec(node->rhs, nullptr, ok)),
                ty);
  case ND_DIV:
  case ND_MOD: {
    int64_t l = eval_rec(node->lhs, nullptr, ok);
    int64_t r = eval_rec(node->rhs, nullptr, ok);
    if (!*ok || r == 0)
      return fail(ok);
    bool div = node->kind == ND_DIV;
    if (is_unsigned_op(ty)) {
      uint64_t ul = ty->size == 4 ? (uint32_t)l : (uint64_t)l;
      uint64_t ur = ty->size == 4 ? (uint32_t)r : (uint64_t)r;
      return wrap((int64_t)(div ? ul / ur : ul % ur), ty);
    }
    if (l == INT64_MIN && r == -1)
      return div ? INT64_MIN : 0;
    return wrap(div ? l / r : l % r, ty);
  }
  case ND_BITAND:
    return eval_rec(node->lhs, nullptr, ok) & eval_rec(node->rhs, nullptr, ok);
  case ND_BITOR:
    return eval_rec(node->lhs, nullptr, ok) | eval_rec(node->rhs, nullptr, ok);
  case ND_BITXOR:
    return eval_rec(node->lhs, nullptr, ok) ^ eval_rec(node->rhs, nullptr, ok);
  case ND_SHL: {
    int64_t l = eval_rec(node->lhs, nullptr, ok);
    int64_t r = eval_rec(node->rhs, nullptr, ok);
    if (r < 0 || r >= 64)
      return 0;
    return wrap((int64_t)((uint64_t)l << r), ty);
  }
  case ND_SHR: {
    int64_t l = eval_rec(node->lhs, nullptr, ok);
    int64_t r = eval_rec(node->rhs, nullptr, ok);
    if (r < 0 || r >= 64)
      return is_unsigned_op(ty) || l >= 0 ? 0 : -1;
    if (is_unsigned_op(ty)) {
      uint64_t ul = ty->size == 4 ? (uint32_t)l : (uint64_t)l;
      return wrap((int64_t)(ul >> r), ty);
    }
    return l >> r;
  }

  case ND_EQ: case ND_NE: case ND_LT: case ND_LE: {
    Type *ot = node->lhs->ty;
    if (is_flonum(ot)) {
      double l = eval_fp(node->lhs, ok), r = eval_fp(node->rhs, ok);
      switch (node->kind) {
      case ND_EQ: return l == r;
      case ND_NE: return l != r;
      case ND_LT: return l < r;
      default: return l <= r;
      }
    }
    int64_t l = eval_rec(node->lhs, nullptr, ok);
    int64_t r = eval_rec(node->rhs, nullptr, ok);
    if (is_unsigned_op(ot)) {
      uint64_t ul = ot->size == 4 ? (uint32_t)l : (uint64_t)l;
      uint64_t ur = ot->size == 4 ? (uint32_t)r : (uint64_t)r;
      switch (node->kind) {
      case ND_EQ: return ul == ur;
      case ND_NE: return ul != ur;
      case ND_LT: return ul < ur;
      default: return ul <= ur;
      }
    }
    switch (node->kind) {
    case ND_EQ: return l == r;
    case ND_NE: return l != r;
    case ND_LT: return l < r;
    default: return l <= r;
    }
  }

  case ND_NEG:
    return wrap((int64_t)(0 - (uint64_t)eval_rec(node->lhs, nullptr, ok)), ty);
  case ND_BITNOT:
    return wrap(~eval_rec(node->lhs, nullptr, ok), ty);
  case ND_NOT:
    return !eval_truth(node->lhs, ok);
  case ND_LOGAND:
    return eval_truth(node->lhs, ok) && eval_truth(node->rhs, ok);
  case ND_LOGOR:
    return eval_truth(node->lhs, ok) || eval_truth(node->rhs, ok);
  case ND_COND:
    return eval_truth(node->cond, ok) ? eval_rec(node->then, label, ok) : eval_rec(node->els, label, ok);
  case ND_COMMA:
    if (node->lhs->ty->kind != TY_VOID)
      eval_rec(node->lhs, nullptr, ok);
    else
      return fail(ok);
    return eval_rec(node->rhs, label, ok);

  case ND_CAST: {
    Node *e = node->lhs;
    if (ty->kind == TY_VOID)
      return fail(ok);
    if (is_flonum(e->ty)) {
      double d = eval_fp(e, ok);
      if (ty->kind == TY_BOOL)
        return d != 0;
      if (ty->is_unsigned && ty->size == 8)
        return (int64_t)(uint64_t)d;
      return wrap((int64_t)d, ty);
    }
    char *inner = nullptr;
    int64_t v = eval_rec(e, label ? &inner : nullptr, ok);
    if (inner) {
      if (ty->kind == TY_BOOL)
        return 1;
      if (ty->size < 8)
        return fail(ok); // a truncated address is not a constant
      *label = inner;
      return v;
    }
    return wrap(v, ty);
  }

  case ND_ADDR:
    return eval_addr(node->lhs, label, ok);

  case ND_VAR:
    if (node->var->has_const_value && is_integer(node->var->ty))
      return wrap(node->var->const_ival, ty);
    return fail(ok);

  default:
    return fail(ok);
  }
}

static double round_to(double v, const Type *ty) { return ty->kind == TY_FLOAT ? (float)v : v; }

static double eval_fp(Node *node, bool *ok) {
  if (!*ok)
    return 0;
  if (is_integer(node->ty)) {
    int64_t v = eval_rec(node, nullptr, ok);
    return node->ty->is_unsigned && node->ty->size == 8 ? (double)(uint64_t)v : (double)v;
  }
  if (!is_flonum(node->ty))
    return fail(ok);

  Type *ty = node->ty;
  switch (node->kind) {
  case ND_NUM:
    return round_to(node->fval, ty);
  case ND_ADD:
    return round_to(eval_fp(node->lhs, ok) + eval_fp(node->rhs, ok), ty);
  case ND_SUB:
    return round_to(eval_fp(node->lhs, ok) - eval_fp(node->rhs, ok), ty);
  case ND_MUL:
    return round_to(eval_fp(node->lhs, ok) * eval_fp(node->rhs, ok), ty);
  case ND_DIV:
    return round_to(eval_fp(node->lhs, ok) / eval_fp(node->rhs, ok), ty);
  case ND_NEG:
    return -eval_fp(node->lhs, ok);
  case ND_COND:
    return eval_truth(node->cond, ok) ? eval_fp(node->then, ok) : eval_fp(node->els, ok);
  case ND_COMMA:
    eval_fp(node->lhs, ok);
    return eval_fp(node->rhs, ok);
  case ND_CAST: {
    Node *e = node->lhs;
    if (is_flonum(e->ty))
      return round_to(eval_fp(e, ok), ty);
    int64_t v = eval_rec(e, nullptr, ok);
    double d = e->ty->is_unsigned && e->ty->size == 8 ? (double)(uint64_t)v : (double)v;
    return round_to(d, ty);
  }
  case ND_VAR:
    if (node->var->has_const_value)
      return node->var->const_fval;
    return fail(ok);
  default:
    return fail(ok);
  }
}

bool is_const_expr(Node *node) {
  bool ok = true;
  if (is_flonum(node->ty)) {
    eval_fp(node, &ok);
  } else {
    char *label = nullptr;
    eval_rec(node, &label, &ok);
  }
  return ok;
}

bool is_const_int_expr(Node *node) {
  bool ok = true;
  eval_rec(node, nullptr, &ok);
  return ok;
}

int64_t eval_int(Node *node) {
  bool ok = true;
  int64_t v = eval_rec(node, nullptr, &ok);
  if (!ok)
    error_tok(node->tok, "expression is not an integer constant expression");
  return v;
}

int64_t eval_reloc(Node *node, char **label) {
  bool ok = true;
  int64_t v = eval_rec(node, label, &ok);
  if (!ok)
    error_tok(node->tok, "initializer element is not a compile-time constant");
  return v;
}

double eval_double(Node *node) {
  bool ok = true;
  double v = eval_fp(node, &ok);
  if (!ok)
    error_tok(node->tok, "expression is not a constant");
  return v;
}

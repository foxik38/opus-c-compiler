// expr.c - expressions (C23 6.5) and their semantic rules.
//
// Every constructor type-checks its operands and inserts the implicit
// conversions the standard requires, so the resulting tree is fully typed.
#include <math.h>

#include "parse/parser.h"

// ---------------------------------------------------------------------------
// Node constructors
// ---------------------------------------------------------------------------

Node *new_node(NodeKind kind, Token *tok) {
  Node *node = NEW(Node);
  node->kind = kind;
  node->tok = tok;
  return node;
}

Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = expr;
  return node;
}

Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

Node *new_num(int64_t val, Type *ty, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty;
  return node;
}

static Node *new_fnum(double val, Type *ty, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->fval = val;
  node->ty = ty;
  return node;
}

Node *new_var_node(Obj *var, Token *tok) {
  Node *node = new_node(ND_VAR, tok);
  node->var = var;
  node->ty = var->ty;
  return node;
}

Node *new_cast(Node *expr, Type *ty) {
  Node *node = new_unary(ND_CAST, expr, expr->tok);
  node->ty = ty;
  return node;
}

// Two scalar types that the code generator treats identically.
static bool same_repr(const Type *a, const Type *b) {
  if (is_pointer_like(a) && is_pointer_like(b))
    return true;
  if (a->kind == TY_BOOL || b->kind == TY_BOOL)
    return a->kind == b->kind;
  if (is_integer(a) && is_integer(b))
    return a->size == b->size && a->is_unsigned == b->is_unsigned;
  return a->kind == b->kind && a->kind != TY_STRUCT && a->kind != TY_UNION && a->kind != TY_ARRAY;
}

static Node *implicit_cast(Node *expr, Type *ty) {
  if (same_repr(expr->ty, ty) && expr->ty->kind != TY_NULLPTR) {
    if (expr->ty == ty || (is_pointer_like(ty) && expr->ty->kind == ty->kind))
      return expr;
    if (!is_pointer_like(ty)) {
      // Keep the node, but give it the (possibly enum/typedef) target type.
      Node *n = new_cast(expr, ty);
      n->is_implicit = true;
      return n;
    }
  }
  Node *n = new_cast(expr, unqualified(ty));
  n->is_implicit = true;
  return n;
}

static void mark_addr_taken(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    node->var->addr_taken = true;
    node->var->writes++;
    return;
  case ND_MEMBER:
    mark_addr_taken(node->lhs);
    return;
  case ND_COMMA:
    mark_addr_taken(node->rhs);
    return;
  default:
    return;
  }
}

// An unsigned int bit-field narrower than int: its values all fit in int, so
// the integer promotions make it int (C23 6.3.1.1), not unsigned int.
static bool is_int_promoted_bitfield(const Node *node) {
  return node->kind == ND_MEMBER && node->member->is_bitfield && is_integer(node->ty) &&
         node->ty->is_unsigned && node->ty->size == 4 && node->member->bit_width < 32;
}

// Lvalue-to-rvalue conversion: arrays and functions decay to pointers, and
// narrow unsigned bit-fields are promoted to int.
Node *rvalue(Node *node) {
  if (is_int_promoted_bitfield(node)) {
    Node *n = new_cast(node, ty_int);
    n->is_implicit = true;
    return n;
  }
  if (node->ty->kind == TY_ARRAY) {
    mark_addr_taken(node);
    Node *n = new_unary(ND_ADDR, node, node->tok);
    n->ty = pointer_to(node->ty->base);
    return n;
  }
  if (node->ty->kind == TY_FUNC) {
    Node *n = new_unary(ND_ADDR, node, node->tok);
    n->ty = pointer_to(node->ty);
    return n;
  }
  return node;
}

bool is_lvalue(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    return node->ty->kind != TY_FUNC;
  case ND_DEREF:
    return true;
  case ND_MEMBER:
    return node->lhs->kind != ND_FUNCALL; // members of rvalue structs are not lvalues
  case ND_COMMA:
    return node->is_compound_literal;
  default:
    return false;
  }
}

bool has_side_effects(Node *node) {
  if (!node)
    return false;
  switch (node->kind) {
  case ND_ASSIGN: case ND_FUNCALL: case ND_VA_START: case ND_VA_ARG: case ND_OVERFLOW:
  case ND_UNREACHABLE: case ND_MEMZERO:
    return true;
  case ND_DEREF:
    if (node->ty->is_volatile)
      return true;
    break;
  case ND_VAR:
    return node->ty->is_volatile;
  default:
    break;
  }
  return has_side_effects(node->lhs) || has_side_effects(node->rhs) || has_side_effects(node->cond) ||
         has_side_effects(node->then) || has_side_effects(node->els);
}

bool is_null_pointer_constant(Node *node) {
  if (node->ty->kind == TY_NULLPTR)
    return true;
  if (node->kind == ND_CAST && !node->is_implicit && node->ty->kind == TY_PTR &&
      node->ty->base->kind == TY_VOID)
    node = node->lhs;
  return is_integer(node->ty) && is_const_int_expr(node) && eval_int(node) == 0;
}

static bool is_const_int(Node *node, int64_t *val) {
  if (!is_integer(node->ty) || !is_const_int_expr(node))
    return false;
  *val = eval_int(node);
  return true;
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

static int64_t wrap_to(int64_t v, const Type *ty) {
  if (ty->kind == TY_BOOL)
    return v != 0;
  int bits = ty->size * 8;
  if (bits >= 64)
    return v;
  uint64_t mask = (1ULL << bits) - 1;
  uint64_t u = (uint64_t)v & mask;
  if (!ty->is_unsigned && (u >> (bits - 1)))
    u |= ~mask;
  return (int64_t)u;
}

// Warns when a constant changes value on implicit conversion (e.g. char c = 300).
static void check_constant_conversion(Node *expr, Type *to, Token *tok) {
  if (to->kind == TY_BOOL)
    return;
  if (is_integer(to) && is_flonum(expr->ty) && is_const_expr(expr)) {
    double d = eval_double(expr);
    double lo = to->is_unsigned ? -1.0 : -ldexp(1.0, to->size * 8 - 1) - 1.0;
    double hi = to->is_unsigned ? ldexp(1.0, to->size * 8) : ldexp(1.0, to->size * 8 - 1);
    if (!(d > lo && d < hi))
      warn_tok(W_OVERFLOW, tok, "conversion of %g to '%s' is out of range (undefined behavior)", d,
               type_name(to));
    return;
  }
  int64_t v;
  if (!is_integer(to) || to->size >= 8 || !is_const_int(expr, &v))
    return;
  int bits = to->size * 8;
  bool fits;
  if (expr->ty->is_unsigned && expr->ty->size == 8 && v < 0)
    fits = false;
  else if (to->is_unsigned)
    fits = v >= -(INT64_C(1) << (bits - 1)) && v < (INT64_C(1) << bits);
  else
    fits = v >= -(INT64_C(1) << (bits - 1)) && v < (INT64_C(1) << (bits - 1));
  if (!fits)
    warn_tok(W_OVERFLOW, tok, "implicit conversion from '%s' to '%s' changes value from %lld to %lld",
             type_name(expr->ty), type_name(to), (long long)v, (long long)wrap_to(v, to));
}

static bool is_void_ptr(const Type *ty) { return ty->kind == TY_PTR && ty->base->kind == TY_VOID; }

static void check_pointer_conversion(Type *from, Type *to, Token *tok, const char *ctx) {
  Type *fb = from->base, *tb = to->base;
  if ((fb->is_const && !tb->is_const) || (fb->is_volatile && !tb->is_volatile)) {
    warn_tok(W_DISCARDED_QUALIFIERS, tok, "%s '%s' from '%s' discards qualifiers", ctx, type_name(to),
             type_name(from));
    return;
  }
  if (is_void_ptr(from) || is_void_ptr(to))
    return;
  if (types_compatible(unqualified(fb), unqualified(tb)))
    return;
  // char * vs unsigned char * and friends: only signedness differs.
  if (is_integer(fb) && is_integer(tb) && fb->size == tb->size && fb->kind != TY_BOOL && tb->kind != TY_BOOL)
    return;
  warn_tok(W_INCOMPATIBLE_POINTER, tok, "incompatible pointer types %s '%s' from '%s'", ctx,
           type_name(to), type_name(from));
}

// Conversion as if by assignment (C23 6.5.16.1), used for assignments,
// initialization, argument passing and return.
Node *convert_for_assign(Node *expr, Type *ty, Token *tok, const char *ctx) {
  expr = rvalue(expr);
  Type *from = expr->ty;

  if (from->kind == TY_VOID)
    error_tok(tok, "%s '%s' from incompatible type 'void'", ctx, type_name(ty));

  if (is_numeric(ty) && is_numeric(from)) {
    check_constant_conversion(expr, ty, tok);
    return implicit_cast(expr, ty);
  }
  if (ty->kind == TY_BOOL && is_pointer_like(from))
    return implicit_cast(expr, ty);

  if (ty->kind == TY_PTR) {
    if (is_null_pointer_constant(expr))
      return implicit_cast(expr, ty);
    if (from->kind == TY_PTR) {
      check_pointer_conversion(from, ty, tok, ctx);
      return implicit_cast(expr, ty);
    }
    if (is_integer(from)) {
      warn_tok(W_INT_CONVERSION, tok, "%s '%s' from '%s' makes pointer from integer without a cast", ctx,
               type_name(ty), type_name(from));
      return implicit_cast(expr, ty);
    }
  }
  if (ty->kind == TY_NULLPTR && is_null_pointer_constant(expr))
    return implicit_cast(expr, ty);

  if (is_integer(ty) && is_pointer_like(from)) {
    warn_tok(W_INT_CONVERSION, tok, "%s '%s' from '%s' makes integer from pointer without a cast", ctx,
             type_name(ty), type_name(from));
    return implicit_cast(expr, ty);
  }

  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && types_compatible(unqualified(ty), unqualified(from)))
    return expr;

  error_tok(tok, "incompatible types when %s '%s' from '%s'", ctx, type_name(ty), type_name(from));
}

// Condition of if/while/for/?:/&&/||/!.
Node *to_condition(Node *node, Token *tok) {
  if (node->kind == ND_ASSIGN && !node->in_parens && tok_equal(node->tok, "="))
    warn_tok(W_PARENTHESES, node->tok, "using the result of an assignment as a condition without parentheses");
  node = rvalue(node);
  if (!is_scalar(node->ty))
    error_tok(tok, "used type '%s' where a scalar is required", type_name(node->ty));
  return node;
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

static void usual_arith_conv(Node **lhs, Node **rhs) {
  Type *ty = common_type((*lhs)->ty, (*rhs)->ty);
  *lhs = implicit_cast(*lhs, ty);
  *rhs = implicit_cast(*rhs, ty);
}

static const char *op_name(Token *tok) { return tok_text(tok); }

[[noreturn]] static void invalid_operands(Node *lhs, Node *rhs, Token *tok) {
  error_tok(tok, "invalid operands to binary expression ('%s' %s '%s')", type_name(lhs->ty), op_name(tok),
            type_name(rhs->ty));
}

// Warns if a constant signed operation overflows (undefined behavior).
static void check_signed_overflow(NodeKind kind, Node *lhs, Node *rhs, Type *ty, Token *tok) {
  int64_t a, b;
  if (ty->is_unsigned || !is_const_int(lhs, &a) || !is_const_int(rhs, &b))
    return;
  int64_t r;
  bool overflow;
  switch (kind) {
  case ND_ADD: overflow = __builtin_add_overflow(a, b, &r); break;
  case ND_SUB: overflow = __builtin_sub_overflow(a, b, &r); break;
  case ND_MUL: overflow = __builtin_mul_overflow(a, b, &r); break;
  default: return;
  }
  if (!overflow && ty->size == 4)
    overflow = r < INT32_MIN || r > INT32_MAX;
  if (overflow)
    warn_tok(W_OVERFLOW, tok, "overflow in expression; result is undefined behavior for type '%s'",
             type_name(ty));
}

static Node *new_arith(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  lhs = rvalue(lhs);
  rhs = rvalue(rhs);
  bool int_only = kind == ND_MOD || kind == ND_BITAND || kind == ND_BITOR || kind == ND_BITXOR ||
                  kind == ND_SHL || kind == ND_SHR;
  bool ok = int_only ? is_integer(lhs->ty) && is_integer(rhs->ty) : is_numeric(lhs->ty) && is_numeric(rhs->ty);
  if (!ok)
    invalid_operands(lhs, rhs, tok);

  if (kind == ND_SHL || kind == ND_SHR) {
    lhs = implicit_cast(lhs, integer_promote(lhs->ty));
    rhs = implicit_cast(rhs, integer_promote(rhs->ty));
    int64_t count;
    if (is_const_int(rhs, &count)) {
      if (count < 0)
        warn_tok(W_SHIFT_COUNT, tok, "shift count is negative (undefined behavior)");
      else if (count >= lhs->ty->size * 8)
        warn_tok(W_SHIFT_COUNT, tok, "shift count >= width of type '%s' (undefined behavior)",
                 type_name(lhs->ty));
    }
    int64_t v;
    if (kind == ND_SHL && is_signed_integer(lhs->ty) && is_const_int(lhs, &v) && v < 0)
      warn_tok(W_SHIFT_COUNT, tok, "shifting a negative signed value is undefined behavior");
    Node *n = new_binary(kind, lhs, rhs, tok);
    n->ty = lhs->ty;
    return n;
  }

  usual_arith_conv(&lhs, &rhs);
  int64_t d;
  if ((kind == ND_DIV || kind == ND_MOD) && is_const_int(rhs, &d) && d == 0)
    warn_tok(W_DIV_BY_ZERO, tok, "division by zero is undefined behavior");
  check_signed_overflow(kind, lhs, rhs, lhs->ty, tok);
  Node *n = new_binary(kind, lhs, rhs, tok);
  n->ty = lhs->ty;
  return n;
}

static void check_pointer_arith(Node *ptr, Token *tok) {
  Type *base = ptr->ty->base;
  if (base->kind == TY_FUNC)
    error_tok(tok, "arithmetic on a pointer to the function type '%s'", type_name(base));
  if (base->kind == TY_VOID)
    return; // GNU extension: sizeof(void) == 1
  if (!is_complete(base))
    error_tok(tok, "arithmetic on a pointer to an incomplete type '%s'", type_name(base));
}

static Node *scale(Node *index, int size, Token *tok) {
  index = implicit_cast(index, ty_long);
  if (size == 1)
    return index;
  Node *n = new_binary(ND_MUL, index, new_num(size, ty_long, tok), tok);
  n->ty = ty_long;
  return n;
}

Node *new_add(Node *lhs, Node *rhs, Token *tok) {
  lhs = rvalue(lhs);
  rhs = rvalue(rhs);
  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_arith(ND_ADD, lhs, rhs, tok);
  if (lhs->ty->kind != TY_PTR && rhs->ty->kind == TY_PTR) {
    Node *t = lhs;
    lhs = rhs;
    rhs = t;
  }
  if (lhs->ty->kind != TY_PTR || !is_integer(rhs->ty))
    invalid_operands(lhs, rhs, tok);
  check_pointer_arith(lhs, tok);
  Node *n = new_binary(ND_ADD, lhs, scale(rhs, lhs->ty->base->size, tok), tok);
  n->ty = unqualified(lhs->ty);
  return n;
}

static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  lhs = rvalue(lhs);
  rhs = rvalue(rhs);
  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_arith(ND_SUB, lhs, rhs, tok);

  if (lhs->ty->kind == TY_PTR && is_integer(rhs->ty)) {
    check_pointer_arith(lhs, tok);
    Node *n = new_binary(ND_SUB, lhs, scale(rhs, lhs->ty->base->size, tok), tok);
    n->ty = unqualified(lhs->ty);
    return n;
  }

  if (lhs->ty->kind == TY_PTR && rhs->ty->kind == TY_PTR) {
    check_pointer_arith(lhs, tok);
    if (!types_compatible(unqualified(lhs->ty->base), unqualified(rhs->ty->base)))
      warn_tok(W_INCOMPATIBLE_POINTER, tok, "subtraction of incompatible pointer types ('%s' and '%s')",
               type_name(lhs->ty), type_name(rhs->ty));
    Node *diff = new_binary(ND_SUB, lhs, rhs, tok);
    diff->ty = ty_long;
    int size = lhs->ty->base->size;
    if (size <= 1)
      return diff;
    Node *n = new_binary(ND_DIV, diff, new_num(size, ty_long, tok), tok);
    n->ty = ty_long;
    return n;
  }
  invalid_operands(lhs, rhs, tok);
}

static bool is_string_literal_ptr(Node *node) {
  while (node->kind == ND_CAST)
    node = node->lhs;
  return node->kind == ND_ADDR && node->lhs->kind == ND_VAR && node->lhs->var->is_string_literal;
}

// True if an integer expression can never be negative, e.g. a promoted
// unsigned char, a comparison, or x & 0x7f. A signed operand like that
// cannot make a mixed-signedness comparison misbehave.
static bool is_known_nonnegative(Node *n) {
  int64_t v;
  if (is_const_int(n, &v))
    return v >= 0;
  if (n->ty->is_unsigned || n->ty->kind == TY_BOOL)
    return true;
  switch (n->kind) {
  case ND_CAST: // widening keeps the value
    if (is_int_promoted_bitfield(n->lhs))
      return true;
    return is_integer(n->lhs->ty) && n->lhs->ty->size < n->ty->size && is_known_nonnegative(n->lhs);
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_NOT: case ND_LOGAND: case ND_LOGOR:
    return true;
  case ND_BITAND:
    return is_known_nonnegative(n->lhs) || is_known_nonnegative(n->rhs);
  case ND_SHR: case ND_MOD:
    return is_known_nonnegative(n->lhs);
  case ND_DIV: case ND_BITOR: case ND_BITXOR:
    return is_known_nonnegative(n->lhs) && is_known_nonnegative(n->rhs);
  case ND_COND:
    return is_known_nonnegative(n->then) && is_known_nonnegative(n->els);
  default:
    return false;
  }
}

// `swapped`: the operands arrive reversed (a > b is parsed as b < a).
static Node *new_compare(NodeKind kind, Node *lhs, Node *rhs, Token *tok, bool swapped) {
  lhs = rvalue(lhs);
  rhs = rvalue(rhs);

  if ((kind == ND_EQ || kind == ND_NE) && (is_string_literal_ptr(lhs) || is_string_literal_ptr(rhs)))
    warn_tok(W_STRING_COMPARE, tok, "comparison against a string literal compares addresses; use strcmp()");

  if (is_numeric(lhs->ty) && is_numeric(rhs->ty)) {
    if (is_integer(lhs->ty) && is_integer(rhs->ty) && common_type(lhs->ty, rhs->ty)->is_unsigned) {
      Node *signed_side = is_signed_integer(integer_promote(lhs->ty))   ? lhs
                          : is_signed_integer(integer_promote(rhs->ty)) ? rhs
                                                                        : nullptr;
      if (signed_side && !is_known_nonnegative(signed_side)) {
        Node *first = swapped ? rhs : lhs, *second = swapped ? lhs : rhs;
        warn_tok(W_SIGN_COMPARE, tok, "comparison of integers of different signs: '%s' and '%s'",
                 type_name(first->ty), type_name(second->ty));
      }
    }
    usual_arith_conv(&lhs, &rhs);
  } else if (is_pointer_like(lhs->ty) && is_pointer_like(rhs->ty)) {
    if (lhs->ty->kind == TY_PTR && rhs->ty->kind == TY_PTR && !is_void_ptr(lhs->ty) && !is_void_ptr(rhs->ty) &&
        !types_compatible(unqualified(lhs->ty->base), unqualified(rhs->ty->base)))
      warn_tok(W_INCOMPATIBLE_POINTER, tok, "comparison of distinct pointer types ('%s' and '%s')",
               type_name(lhs->ty), type_name(rhs->ty));
  } else if (is_pointer_like(lhs->ty) && is_integer(rhs->ty)) {
    if (!is_null_pointer_constant(rhs))
      warn_tok(W_INT_CONVERSION, tok, "comparison between pointer and integer ('%s' and '%s')",
               type_name(lhs->ty), type_name(rhs->ty));
    rhs = implicit_cast(rhs, lhs->ty);
  } else if (is_integer(lhs->ty) && is_pointer_like(rhs->ty)) {
    if (!is_null_pointer_constant(lhs))
      warn_tok(W_INT_CONVERSION, tok, "comparison between pointer and integer ('%s' and '%s')",
               type_name(lhs->ty), type_name(rhs->ty));
    lhs = implicit_cast(lhs, rhs->ty);
  } else {
    invalid_operands(lhs, rhs, tok);
  }

  Node *n = new_binary(kind, lhs, rhs, tok);
  n->ty = ty_int;
  return n;
}

static Node *new_logical(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *n = new_binary(kind, to_condition(lhs, tok), to_condition(rhs, tok), tok);
  n->ty = ty_int;
  return n;
}

Node *new_deref(Node *expr, Token *tok) {
  expr = rvalue(expr);
  if (expr->ty->kind != TY_PTR)
    error_tok(tok, "indirection requires pointer operand ('%s' invalid)", type_name(expr->ty));
  if (is_null_pointer_constant(expr) || (expr->kind == ND_CAST && is_integer(expr->lhs->ty) &&
                                         is_const_int_expr(expr->lhs) && eval_int(expr->lhs) == 0))
    warn_tok(W_NULL_DEREFERENCE, tok, "dereferencing a null pointer is undefined behavior");
  Node *n = new_unary(ND_DEREF, expr, tok);
  n->ty = expr->ty->base;
  return n;
}

// ---------------------------------------------------------------------------
// Assignment
// ---------------------------------------------------------------------------

static bool has_const_member(const Type *ty) {
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
    return false;
  for (Member *m = ty->members; m; m = m->next)
    if (m->ty->is_const || has_const_member(m->ty))
      return true;
  return false;
}

static void check_assignable(Node *lhs, Token *tok) {
  if (!is_lvalue(lhs))
    error_tok(tok, "expression is not assignable");
  if (lhs->ty->kind == TY_ARRAY)
    error_tok(tok, "array type '%s' is not assignable", type_name(lhs->ty));
  if (lhs->ty->is_const || has_const_member(lhs->ty)) {
    if (lhs->kind == ND_VAR)
      error_tok(tok, "cannot assign to variable '%s' with const-qualified type '%s'", lhs->var->name,
                type_name(lhs->ty));
    error_tok(tok, "cannot assign to an object of const-qualified type '%s'", type_name(lhs->ty));
  }
}

static void mark_written(Node *lhs) {
  if (lhs->kind == ND_VAR)
    lhs->var->writes++;
}

Node *new_assign(Node *lhs, Node *rhs, Token *tok) {
  check_assignable(lhs, tok);
  rhs = convert_for_assign(rhs, lhs->ty, tok, "assigning to");
  mark_written(lhs);
  Node *n = new_binary(ND_ASSIGN, lhs, rhs, tok);
  n->ty = unqualified(lhs->ty);
  return n;
}

// Clones a side-effect-free lvalue so that it can be evaluated twice.
static bool is_pure_path(Node *n) {
  switch (n->kind) {
  case ND_VAR:
    return true;
  case ND_MEMBER:
    return is_pure_path(n->lhs);
  case ND_DEREF:
    return n->lhs->kind == ND_VAR;
  default:
    return false;
  }
}

static Node *clone_path(Node *n) {
  Node *c = NEW(Node);
  *c = *n;
  if (n->kind != ND_VAR)
    c->lhs = clone_path(n->lhs);
  return c;
}

// Returns an lvalue equivalent to `lhs` that may be evaluated more than once;
// side effects are moved into *setup (e.g. "tmp = &a[i++]").
static Node *reusable_lvalue(Node *lhs, Node **setup, Token *tok) {
  *setup = nullptr;
  if (is_pure_path(lhs))
    return lhs;

  Node *target = lhs;
  Member *bitfield = nullptr;
  if (lhs->kind == ND_MEMBER && lhs->member->is_bitfield) {
    target = lhs->lhs; // bit-fields have no address: take the struct's
    bitfield = lhs->member;
  }

  Obj *tmp = new_temp(pointer_to(target->ty));
  Node *addr = new_unary(ND_ADDR, target, tok);
  addr->ty = tmp->ty;
  mark_addr_taken(target);
  Node *asg = new_binary(ND_ASSIGN, new_var_node(tmp, tok), addr, tok);
  asg->ty = tmp->ty;
  *setup = asg;

  Node *deref = new_unary(ND_DEREF, new_var_node(tmp, tok), tok);
  deref->ty = target->ty;
  if (!bitfield)
    return deref;
  Node *mem = new_unary(ND_MEMBER, deref, tok);
  mem->member = bitfield;
  mem->ty = lhs->ty;
  return mem;
}

static Node *with_setup(Node *setup, Node *expr) {
  if (!setup)
    return expr;
  Node *n = new_binary(ND_COMMA, setup, expr, expr->tok);
  n->ty = expr->ty;
  return n;
}

static Node *compound_assign(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  check_assignable(lhs, tok);
  Node *setup;
  Node *target = reusable_lvalue(lhs, &setup, tok);
  Node *value;
  if (kind == ND_ADD)
    value = new_add(clone_path(target), rhs, tok);
  else if (kind == ND_SUB)
    value = new_sub(clone_path(target), rhs, tok);
  else
    value = new_arith(kind, clone_path(target), rhs, tok);
  return with_setup(setup, new_assign(clone_path(target), value, tok));
}

// Post-increment/decrement.
static Node *new_post_incdec(Node *lhs, int delta, Token *tok) {
  check_assignable(lhs, tok);
  Type *ty = unqualified(lhs->ty);
  if (!is_scalar(ty) || ty->kind == TY_NULLPTR)
    error_tok(tok, "cannot increment value of type '%s'", type_name(ty));
  Node *setup;
  Node *target = reusable_lvalue(lhs, &setup, tok);

  Node *result;
  if ((is_integer(ty) && ty->kind != TY_BOOL) || ty->kind == TY_PTR) {
    // (typeof x)((x += d) - d): no temporary needed.
    Node *inc = new_assign(clone_path(target),
                           new_add(clone_path(target), new_num(delta, ty_int, tok), tok), tok);
    result = implicit_cast(new_add(inc, new_num(-delta, ty_int, tok), tok), ty);
  } else {
    // Floating point and bool: tmp = x, x = tmp + d, tmp.
    Obj *tmp = new_temp(ty);
    Node *save = new_binary(ND_ASSIGN, new_var_node(tmp, tok), clone_path(target), tok);
    save->ty = ty;
    Node *upd = new_assign(clone_path(target), new_add(new_var_node(tmp, tok), new_num(delta, ty_int, tok), tok), tok);
    Node *c1 = new_binary(ND_COMMA, upd, new_var_node(tmp, tok), tok);
    c1->ty = ty;
    result = new_binary(ND_COMMA, save, c1, tok);
    result->ty = ty;
  }
  return with_setup(setup, result);
}

// ---------------------------------------------------------------------------
// Member access and calls
// ---------------------------------------------------------------------------

static Member *find_member(Type *ty, Token *name, Member **anon) {
  *anon = nullptr;
  for (Member *m = ty->members; m; m = m->next) {
    if (m->name && m->name->len == name->len && !memcmp(m->name->loc, name->loc, (size_t)name->len))
      return m;
    if (!m->name && (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION)) {
      Member *inner;
      if (find_member(m->ty, name, &inner)) {
        *anon = m;
        return m;
      }
    }
  }
  return nullptr;
}

static Node *member_node(Node *lhs, Member *m, Token *tok) {
  Node *n = new_unary(ND_MEMBER, lhs, tok);
  n->member = m;
  n->ty = qualified(m->ty, lhs->ty->is_const, lhs->ty->is_volatile);
  return n;
}

Node *member_access(Node *lhs, Token *name) {
  if (lhs->ty->kind != TY_STRUCT && lhs->ty->kind != TY_UNION)
    error_tok(name, "member reference base type '%s' is not a structure or union", type_name(lhs->ty));
  if (lhs->ty->size < 0)
    error_tok(name, "incomplete definition of type '%s'", type_name(lhs->ty));
  for (;;) {
    Member *anon;
    Member *m = find_member(lhs->ty, name, &anon);
    if (!m)
      error_tok(name, "no member named '%.*s' in '%s'", name->len, name->loc, type_name(lhs->ty));
    lhs = member_node(lhs, m, name);
    if (!anon)
      return lhs;
  }
}

static Node *default_promote(Node *arg) {
  arg = rvalue(arg);
  if (arg->ty->kind == TY_FLOAT)
    return implicit_cast(arg, ty_double);
  if (is_integer(arg->ty))
    return implicit_cast(arg, integer_promote(arg->ty));
  if (arg->ty->kind == TY_NULLPTR)
    return implicit_cast(arg, pointer_to(ty_void));
  return arg;
}

static int count_params(Type *ft) {
  int n = 0;
  for (Type *p = ft->params; p; p = p->next)
    n++;
  return n;
}

// glibc maps scanf to __isoc99_scanf/__isoc23_scanf with macros.
static const char *format_function_name(const char *name) {
  if (starts_with(name, "__isoc99_") || starts_with(name, "__isoc23_"))
    return name + 9;
  return name;
}

static bool is_format_function(const char *name) {
  static const char *const names[] = {"printf", "fprintf", "sprintf", "snprintf", "dprintf",
                                      "scanf",  "fscanf",  "sscanf"};
  name = format_function_name(name);
  for (size_t i = 0; i < ARRAY_LEN(names); i++)
    if (strcmp(name, names[i]) == 0)
      return true;
  return false;
}

static Node *funcall(Token **rest, Token *tok, Node *fn) {
  Token *start = tok;
  fn = rvalue(fn);
  if (fn->ty->kind != TY_PTR || fn->ty->base->kind != TY_FUNC)
    error_tok(fn->tok, "called object type '%s' is not a function or function pointer", type_name(fn->ty));

  Type *ft = fn->ty->base;
  Obj *callee = fn->kind == ND_ADDR && fn->lhs->kind == ND_VAR ? fn->lhs->var : nullptr;
  const char *fname = callee ? callee->name : "function";

  Node head = {};
  Node *cur = &head;
  Type *param = ft->params;
  int argno = 0;
  VEC(Token *) arg_toks = {};
  tok = tok->next;
  while (!tok_equal(tok, ")")) {
    if (argno)
      tok = tok_skip(tok, ",");
    Token *arg_tok = tok;
    vec_push(&arg_toks, arg_tok);
    Node *arg = assign(&tok, tok);
    argno++;
    if (param) {
      char *ctx = format("passing argument %d of '%s' to parameter of type", argno, fname);
      arg = convert_for_assign(arg, unqualified(param), arg_tok, ctx);
      param = param->next;
    } else if (!ft->is_variadic) {
      error_tok(arg_tok, "too many arguments to function call, expected %d, have %d", count_params(ft),
                argno);
    } else {
      arg = default_promote(arg);
      if (arg->ty->kind == TY_VOID)
        error_tok(arg_tok, "argument may not have 'void' type");
    }
    cur = cur->next = arg;
  }
  if (param) {
    int expected = count_params(ft);
    error_tok(tok, "too few arguments to function call, expected %s%d, have %d", ft->is_variadic ? "at least " : "",
              expected, argno);
  }
  *rest = tok->next;

  Node *n = new_node(ND_FUNCALL, start);
  n->lhs = fn;
  n->func_ty = ft;
  n->ty = unqualified(ft->return_ty);
  n->args = head.next;
  if ((n->ty->kind == TY_STRUCT || n->ty->kind == TY_UNION)) {
    if (!is_complete(n->ty))
      error_tok(start, "calling function with incomplete return type '%s'", type_name(n->ty));
    if (P.current_fn)
      n->ret_buffer = new_temp(n->ty);
  }
  if (callee && is_format_function(callee->name))
    check_format_call(n, format_function_name(callee->name), arg_toks.data);
  return n;
}

// ---------------------------------------------------------------------------
// Grammar: expression ... primary
// ---------------------------------------------------------------------------

static Node *logor(Token **rest, Token *tok);
static Node *postfix_tail(Token **rest, Token *tok, Node *node);
static Node *cast_expr(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);

Node *expr(Token **rest, Token *tok) {
  Node *node = assign(&tok, tok);
  while (tok_equal(tok, ",")) {
    Token *op = tok;
    note_discarded(node);
    Node *rhs = rvalue(assign(&tok, tok->next));
    node = new_binary(ND_COMMA, rvalue(node), rhs, op);
    node->ty = rhs->ty;
  }
  *rest = tok;
  return node;
}

int64_t const_expr(Token **rest, Token *tok) {
  Token *start = tok;
  Node *node = conditional(rest, tok);
  if (!is_integer(node->ty))
    error_tok(start, "integer constant expression has type '%s'", type_name(node->ty));
  if (!is_const_int_expr(node))
    error_tok(start, "expression is not an integer constant expression");
  return eval_int(node);
}

// The value of `e` is thrown away (expression statement, left operand of a
// comma, cast to void, for-loop increment). A plain assignment to a variable
// there only sets it, which is what "set but not used" counts.
void note_discarded(Node *e) {
  while (e->kind == ND_CAST && e->is_implicit) // (void) casts note their operand themselves
    e = e->lhs;
  switch (e->kind) {
  case ND_ASSIGN:
    if (e->lhs->kind == ND_VAR && tok_equal(e->tok, "="))
      e->lhs->var->lhs_refs++;
    return;
  case ND_COMMA:
    if (!e->is_compound_literal)
      note_discarded(e->rhs); // the left operand was noted when the comma was built
    return;
  case ND_COND:
    note_discarded(e->then);
    note_discarded(e->els);
    return;
  default:
    return;
  }
}

Node *assign(Token **rest, Token *tok) {
  Node *node = conditional(&tok, tok);

  if (tok_equal(tok, "=")) {
    Token *op = tok;
    Node *rhs = assign(rest, tok->next);
    if (node->kind == ND_VAR && node->var->first_read == node->tok)
      node->var->first_read = nullptr; // the target of '=' is not a read
    return new_assign(node, rhs, op);
  }

  static const struct {
    const char *op;
    NodeKind kind;
  } ops[] = {
      {"+=", ND_ADD},    {"-=", ND_SUB},   {"*=", ND_MUL},    {"/=", ND_DIV},  {"%=", ND_MOD},
      {"&=", ND_BITAND}, {"|=", ND_BITOR}, {"^=", ND_BITXOR}, {"<<=", ND_SHL}, {">>=", ND_SHR},
  };
  for (size_t i = 0; i < ARRAY_LEN(ops); i++) {
    if (tok_equal(tok, ops[i].op)) {
      Token *op = tok;
      Node *rhs = assign(rest, tok->next);
      return compound_assign(ops[i].kind, node, rhs, op);
    }
  }

  *rest = tok;
  return node;
}

static bool is_void_expr(Node *n) { return n->ty->kind == TY_VOID; }

static Node *new_conditional(Node *cond, Node *then, Node *els, Token *tok) {
  then = rvalue(then);
  els = rvalue(els);
  Type *ty;
  Type *a = then->ty, *b = els->ty;

  if (is_numeric(a) && is_numeric(b)) {
    usual_arith_conv(&then, &els);
    ty = then->ty;
  } else if (is_void_expr(then) && is_void_expr(els)) {
    ty = ty_void;
  } else if ((a->kind == TY_STRUCT || a->kind == TY_UNION) && types_compatible(unqualified(a), unqualified(b))) {
    ty = unqualified(a);
  } else if (a->kind == TY_PTR && (b->kind == TY_NULLPTR || is_null_pointer_constant(els))) {
    ty = a;
    els = implicit_cast(els, ty);
  } else if (b->kind == TY_PTR && (a->kind == TY_NULLPTR || is_null_pointer_constant(then))) {
    ty = b;
    then = implicit_cast(then, ty);
  } else if (a->kind == TY_NULLPTR && b->kind == TY_NULLPTR) {
    ty = a;
  } else if (a->kind == TY_PTR && b->kind == TY_PTR) {
    bool c = a->base->is_const || b->base->is_const;
    bool v = a->base->is_volatile || b->base->is_volatile;
    if (is_void_ptr(a) || is_void_ptr(b)) {
      ty = pointer_to(qualified(ty_void, c, v));
    } else if (types_compatible(unqualified(a->base), unqualified(b->base))) {
      ty = pointer_to(qualified(unqualified(a->base), c, v));
    } else {
      warn_tok(W_INCOMPATIBLE_POINTER, tok, "pointer type mismatch ('%s' and '%s')", type_name(a), type_name(b));
      ty = pointer_to(ty_void);
    }
    then = implicit_cast(then, ty);
    els = implicit_cast(els, ty);
  } else if ((a->kind == TY_PTR && is_integer(b)) || (is_integer(a) && b->kind == TY_PTR)) {
    warn_tok(W_INT_CONVERSION, tok, "pointer/integer type mismatch in conditional expression ('%s' and '%s')",
             type_name(a), type_name(b));
    ty = a->kind == TY_PTR ? a : b;
    then = implicit_cast(then, ty);
    els = implicit_cast(els, ty);
  } else {
    error_tok(tok, "incompatible operand types ('%s' and '%s')", type_name(a), type_name(b));
  }

  Node *n = new_node(ND_COND, tok);
  n->cond = cond;
  n->then = then;
  n->els = els;
  n->ty = ty;
  return n;
}

Node *conditional(Token **rest, Token *tok) {
  Node *cond = logor(&tok, tok);
  if (!tok_equal(tok, "?")) {
    *rest = tok;
    return cond;
  }
  Token *q = tok;
  cond = to_condition(cond, q);
  Node *then = expr(&tok, tok->next);
  tok = tok_skip(tok, ":");
  Node *els = conditional(rest, tok);
  return new_conditional(cond, then, els, q);
}

// Binary operators by precedence level, lowest first.
typedef struct {
  const char *op;
  NodeKind kind;
} BinOp;

static Node *binary_level(Token **rest, Token *tok, int level);

static const BinOp *level_ops(int level, int *count) {
  static const BinOp logand_ops[] = {{"&&", ND_LOGAND}};
  static const BinOp bitor_ops[] = {{"|", ND_BITOR}};
  static const BinOp bitxor_ops[] = {{"^", ND_BITXOR}};
  static const BinOp bitand_ops[] = {{"&", ND_BITAND}};
  static const BinOp eq_ops[] = {{"==", ND_EQ}, {"!=", ND_NE}};
  static const BinOp rel_ops[] = {{"<", ND_LT}, {"<=", ND_LE}, {">", ND_LT}, {">=", ND_LE}};
  static const BinOp shift_ops[] = {{"<<", ND_SHL}, {">>", ND_SHR}};
  static const BinOp add_ops[] = {{"+", ND_ADD}, {"-", ND_SUB}};
  static const BinOp mul_ops[] = {{"*", ND_MUL}, {"/", ND_DIV}, {"%", ND_MOD}};
  static const struct {
    const BinOp *ops;
    int n;
  } levels[] = {
      {logand_ops, 1}, {bitor_ops, 1}, {bitxor_ops, 1}, {bitand_ops, 1}, {eq_ops, 2},
      {rel_ops, 4},    {shift_ops, 2}, {add_ops, 2},    {mul_ops, 3},
  };
  if (level >= (int)ARRAY_LEN(levels))
    return nullptr;
  *count = levels[level].n;
  return levels[level].ops;
}

static Node *make_binary(const BinOp *op, Node *lhs, Node *rhs, Token *tok) {
  switch (op->kind) {
  case ND_LOGAND:
    return new_logical(ND_LOGAND, lhs, rhs, tok);
  case ND_EQ: case ND_NE:
    return new_compare(op->kind, lhs, rhs, tok, false);
  case ND_LT: case ND_LE:
    // a > b is b < a, a >= b is b <= a.
    if (op->op[0] == '>')
      return new_compare(op->kind, rhs, lhs, tok, true);
    return new_compare(op->kind, lhs, rhs, tok, false);
  case ND_ADD:
    return new_add(lhs, rhs, tok);
  case ND_SUB:
    return new_sub(lhs, rhs, tok);
  default:
    return new_arith(op->kind, lhs, rhs, tok);
  }
}

static Node *binary_level(Token **rest, Token *tok, int level) {
  int n;
  const BinOp *ops = level_ops(level, &n);
  if (!ops)
    return cast_expr(rest, tok);

  Node *node = binary_level(&tok, tok, level + 1);
  for (;;) {
    const BinOp *match = nullptr;
    for (int i = 0; i < n; i++)
      if (tok_equal(tok, ops[i].op))
        match = &ops[i];
    if (!match) {
      *rest = tok;
      return node;
    }
    Token *op = tok;
    Node *rhs = binary_level(&tok, tok->next, level + 1);
    node = make_binary(match, node, rhs, op);
  }
}

static Node *logor(Token **rest, Token *tok) {
  Node *node = binary_level(&tok, tok, 0);
  while (tok_equal(tok, "||")) {
    Token *op = tok;
    Node *rhs = binary_level(&tok, tok->next, 0);
    node = new_logical(ND_LOGOR, node, rhs, op);
  }
  *rest = tok;
  return node;
}

static Node *compound_literal(Token **rest, Token *tok, Type *ty, Token *start) {
  if (ty->kind == TY_FUNC || ty->kind == TY_VOID)
    error_tok(start, "compound literal has invalid type '%s'", type_name(ty));
  if (!P.current_fn) {
    Obj *var = new_anon_gvar(ty);
    gvar_initializer(rest, tok, var);
    return new_var_node(var, start);
  }
  Obj *var = new_temp(ty);
  Node *init = lvar_initializer(rest, tok, var);
  var->align = MAX(var->align, var->ty->align);
  Node *n = new_binary(ND_COMMA, init, new_var_node(var, start), start);
  n->ty = var->ty;
  n->is_compound_literal = true;
  return n;
}

static Node *explicit_cast(Node *e, Type *ty, Token *tok) {
  e = rvalue(e);
  if (ty->kind == TY_VOID) {
    note_discarded(e);
    Node *n = new_cast(e, ty_void);
    n->tok = tok;
    return n;
  }
  if (!is_scalar(ty))
    error_tok(tok, "used type '%s' where arithmetic or pointer type is required", type_name(ty));
  if (!is_scalar(e->ty))
    error_tok(tok, "operand of type '%s' where arithmetic or pointer type is required", type_name(e->ty));
  if ((is_flonum(ty) && is_pointer_like(e->ty)) || (is_pointer_like(ty) && is_flonum(e->ty)))
    error_tok(tok, "cannot cast between pointer type and floating type");
  if (ty->kind == TY_NULLPTR && !is_null_pointer_constant(e))
    error_tok(tok, "cannot cast a non-null value to 'nullptr_t'");
  Node *n = new_cast(e, unqualified(ty));
  n->tok = tok;
  return n;
}

static Node *cast_expr(Token **rest, Token *tok) {
  if (tok_equal(tok, "(") && is_typename(tok->next)) {
    Token *start = tok;
    Type *ty = typename_(&tok, tok->next);
    tok = tok_skip(tok, ")");
    if (tok_equal(tok, "{")) {
      // Compound literal, possibly followed by postfix operators.
      Node *lit = compound_literal(&tok, tok, ty, start);
      return postfix_tail(rest, tok, lit);
    }
    Node *e = cast_expr(rest, tok);
    return explicit_cast(e, ty, start);
  }
  return unary(rest, tok);
}

static Node *address_of(Node *e, Token *tok) {
  if (e->kind == ND_MEMBER && e->member->is_bitfield)
    error_tok(tok, "address of bit-field requested");
  if (e->kind == ND_VAR && e->var->is_register)
    error_tok(tok, "address of register variable '%s' requested", e->var->name);
  if (e->kind == ND_DEREF) {
    // &*p is p (C23 6.5.3.2), without the dereference.
    Node *p = e->lhs;
    if (p->ty->base == e->ty || p->ty->kind == TY_PTR)
      return p;
  }
  if (!is_lvalue(e) && e->ty->kind != TY_FUNC)
    error_tok(tok, "cannot take the address of an rvalue of type '%s'", type_name(e->ty));
  mark_addr_taken(e);
  Node *n = new_unary(ND_ADDR, e, tok);
  n->ty = pointer_to(e->ty);
  return n;
}

static Node *sizeof_operand(Token **rest, Token *tok, Type **ty) {
  P.unevaluated++;
  Node *e = unary(rest, tok);
  P.unevaluated--;
  *ty = e->ty;
  return e;
}

static Node *unary(Token **rest, Token *tok) {
  Token *op = tok;
  if (tok_equal(tok, "+") || tok_equal(tok, "-") || tok_equal(tok, "~")) {
    Node *e = rvalue(cast_expr(rest, tok->next));
    bool is_bitnot = tok_equal(op, "~");
    if (is_bitnot ? !is_integer(e->ty) : !is_numeric(e->ty))
      error_tok(op, "invalid argument type '%s' to unary expression", type_name(e->ty));
    e = implicit_cast(e, integer_promote(e->ty));
    if (tok_equal(op, "+"))
      return e;
    Node *n = new_unary(is_bitnot ? ND_BITNOT : ND_NEG, e, op);
    n->ty = e->ty;
    return n;
  }
  if (tok_equal(tok, "!")) {
    Node *e = to_condition(cast_expr(rest, tok->next), op);
    Node *n = new_unary(ND_NOT, e, op);
    n->ty = ty_int;
    return n;
  }
  if (tok_equal(tok, "&")) {
    P.addr_of++;
    Node *e = cast_expr(rest, tok->next);
    P.addr_of--;
    return address_of(e, op);
  }
  if (tok_equal(tok, "*")) {
    Node *e = cast_expr(rest, tok->next);
    return new_deref(e, op);
  }
  if (tok_equal(tok, "++") || tok_equal(tok, "--")) {
    Node *e = unary(rest, tok->next);
    Type *ty = e->ty;
    if (!is_scalar(ty) || ty->kind == TY_NULLPTR)
      error_tok(op, "cannot increment value of type '%s'", type_name(ty));
    return compound_assign(tok_equal(op, "++") ? ND_ADD : ND_SUB, e, new_num(1, ty_int, op), op);
  }

  if (tok_equal(tok, "sizeof")) {
    Type *ty;
    if (tok_equal(tok->next, "(") && is_typename(tok->next->next)) {
      ty = typename_(&tok, tok->next->next);
      tok = tok_skip(tok, ")");
      if (tok_equal(tok, "{")) { // sizeof (T){...}
        P.unevaluated++;
        Node *lit = compound_literal(&tok, tok, ty, op);
        P.unevaluated--;
        ty = lit->ty;
      }
      *rest = tok;
    } else {
      Node *e = sizeof_operand(rest, tok->next, &ty);
      if (e->kind == ND_VAR && e->var->is_param && e->var->ty->param_was_array)
        warn_tok(W_SIZEOF_ARRAY_ARGUMENT, e->tok,
                 "'sizeof' on array function parameter '%s' will return size of '%s'", e->var->name,
                 type_name(e->ty));
      if (e->kind == ND_MEMBER && e->member->is_bitfield)
        error_tok(op, "invalid application of 'sizeof' to a bit-field");
    }
    if (ty->kind == TY_FUNC)
      error_tok(op, "invalid application of 'sizeof' to a function type");
    if (ty->kind == TY_VOID || !is_complete(ty))
      error_tok(op, "invalid application of 'sizeof' to an incomplete type '%s'", type_name(ty));
    return new_num(ty->size, ty_ulong, op);
  }

  if (tok_equal(tok, "alignof") || tok_equal(tok, "_Alignof") || tok_equal(tok, "__alignof__")) {
    Type *ty;
    if (tok_equal(tok->next, "(") && is_typename(tok->next->next)) {
      ty = typename_(&tok, tok->next->next);
      *rest = tok_skip(tok, ")");
    } else {
      sizeof_operand(rest, tok->next, &ty);
    }
    if (ty->kind == TY_ARRAY)
      ty = ty->base;
    return new_num(ty->align, ty_ulong, op);
  }

  return postfix(rest, tok);
}

static void check_array_bounds(Node *base, Node *index, Token *tok) {
  // Only direct array objects have a known length.
  if (base->ty->kind != TY_ARRAY || base->ty->array_len < 0)
    return;
  if (base->kind == ND_MEMBER && !base->member->next && base->ty->array_len <= 1)
    return; // trailing "struct hack" arrays
  int64_t i;
  if (!is_const_int(index, &i))
    return;
  int64_t len = base->ty->array_len;
  if (i < 0 || i > len || (i == len && !P.addr_of))
    warn_tok(W_ARRAY_BOUNDS, tok, "array index %lld is past the end of the array (which contains %lld element%s)",
             (long long)i, (long long)len, len == 1 ? "" : "s");
}

static Node *postfix_tail(Token **rest, Token *tok, Node *node) {
  for (;;) {
    if (tok_equal(tok, "(")) {
      node = funcall(&tok, tok, node);
      continue;
    }

    if (tok_equal(tok, "[")) {
      Token *start = tok;
      Node *index = expr(&tok, tok->next);
      tok = tok_skip(tok, "]");
      check_array_bounds(node, index, start);
      node = new_deref(new_add(node, index, start), start);
      continue;
    }

    if (tok_equal(tok, ".")) {
      node = member_access(node, tok->next);
      tok = tok->next->next;
      continue;
    }

    if (tok_equal(tok, "->")) {
      node = member_access(new_deref(node, tok), tok->next);
      tok = tok->next->next;
      continue;
    }

    if (tok_equal(tok, "++") || tok_equal(tok, "--")) {
      node = new_post_incdec(node, tok_equal(tok, "++") ? 1 : -1, tok);
      tok = tok->next;
      continue;
    }

    *rest = tok;
    return node;
  }
}

static Node *postfix(Token **rest, Token *tok) {
  Node *node = primary(&tok, tok);
  return postfix_tail(rest, tok, node);
}

// ---------------------------------------------------------------------------
// Primary expressions and builtins
// ---------------------------------------------------------------------------

static Type *literal_type(LiteralType lit) {
  switch (lit) {
  case LIT_INT: return ty_int;
  case LIT_UINT: return ty_uint;
  case LIT_LONG: return ty_long;
  case LIT_ULONG: return ty_ulong;
  case LIT_LLONG: return ty_llong;
  case LIT_ULLONG: return ty_ullong;
  case LIT_UCHAR: return ty_uchar;
  case LIT_USHORT: return ty_ushort;
  case LIT_FLOAT: return ty_float;
  case LIT_DOUBLE: return ty_double;
  case LIT_LDOUBLE: return ty_ldouble;
  }
  ICE_UNREACHABLE();
}

static Node *generic_selection(Token **rest, Token *tok) {
  Token *start = tok;
  tok = tok_skip(tok->next, "(");
  P.unevaluated++;
  Node *ctrl = rvalue(assign(&tok, tok));
  P.unevaluated--;
  Type *t = unqualified(ctrl->ty);

  Node *result = nullptr, *def = nullptr;
  while (!tok_consume(&tok, tok, ")")) {
    tok = tok_skip(tok, ",");
    if (tok_equal(tok, "default")) {
      tok = tok_skip(tok->next, ":");
      def = assign(&tok, tok);
      continue;
    }
    Token *type_tok = tok;
    Type *at = typename_(&tok, tok);
    tok = tok_skip(tok, ":");
    Node *e = assign(&tok, tok);
    if (types_compatible(t, at)) {
      if (result)
        error_tok(type_tok, "type '%s' in generic association compatible with previously specified type",
                  type_name(at));
      result = e;
    }
  }
  *rest = tok;
  if (!result)
    result = def;
  if (!result)
    error_tok(start, "controlling expression type '%s' not compatible with any generic association type",
              type_name(t));
  return result;
}

static Node *func_name_var(Token *tok) {
  if (!P.current_fn)
    error_tok(tok, "'%.*s' is only valid inside a function", tok->len, tok->loc);
  Obj *fn = P.current_fn;
  if (!fn->func_name) {
    int len = (int)strlen(fn->name) + 1;
    Obj *var = new_anon_gvar(array_of(qualified(ty_char, true, false), len));
    var->init_data = xstrdup(fn->name);
    var->is_string_literal = true;
    fn->func_name = var;
  }
  return new_var_node(fn->func_name, tok);
}

// Builtin names that look like function calls.
static Node *builtin_call(Token **rest, Token *tok, bool *handled) {
  *handled = true;
  Token *start = tok;

  if (tok_equal(tok, "__builtin_va_start")) {
    tok = tok_skip(tok->next, "(");
    Node *ap = rvalue(assign(&tok, tok));
    while (tok_consume(&tok, tok, ","))
      assign(&tok, tok); // C23 allows (and ignores) a second argument
    *rest = tok_skip(tok, ")");
    if (!P.current_fn || !P.current_fn->ty->is_variadic)
      error_tok(start, "'va_start' used in function with fixed arguments");
    Node *n = new_unary(ND_VA_START, ap, start);
    n->ty = ty_void;
    return n;
  }

  if (tok_equal(tok, "__builtin_va_arg")) {
    tok = tok_skip(tok->next, "(");
    Node *ap = rvalue(assign(&tok, tok));
    tok = tok_skip(tok, ",");
    Type *ty = typename_(&tok, tok);
    *rest = tok_skip(tok, ")");
    if (!is_complete(ty))
      error_tok(start, "second argument to 'va_arg' is of incomplete type '%s'", type_name(ty));
    if (is_integer(ty) && ty->size < 4)
      warn_tok(W_UNSUPPORTED, start, "'%s' is promoted to 'int' when passed through '...'", type_name(ty));
    if (ty->kind == TY_FLOAT)
      warn_tok(W_UNSUPPORTED, start, "'float' is promoted to 'double' when passed through '...'");
    Node *n = new_unary(ND_VA_ARG, ap, start);
    n->ty = pointer_to(ty);
    return new_deref(n, start);
  }

  if (tok_equal(tok, "__builtin_offsetof")) {
    tok = tok_skip(tok->next, "(");
    Type *ty = typename_(&tok, tok);
    tok = tok_skip(tok, ",");
    // Build &((T *)0)->designator and evaluate it.
    Node *base = new_cast(new_num(0, ty_long, start), pointer_to(ty));
    Node *node = member_access(new_deref(base, start), tok);
    tok = tok->next;
    for (;;) {
      if (tok_equal(tok, ".")) {
        node = member_access(node, tok->next);
        tok = tok->next->next;
      } else if (tok_equal(tok, "[")) {
        Node *idx = expr(&tok, tok->next);
        tok = tok_skip(tok, "]");
        node = new_deref(new_add(node, idx, start), start);
      } else {
        break;
      }
    }
    *rest = tok_skip(tok, ")");
    if (node->kind == ND_MEMBER && node->member->is_bitfield)
      error_tok(start, "cannot compute offset of bit-field");
    Node *addr = new_unary(ND_ADDR, node, start);
    addr->ty = pointer_to(node->ty);
    char *label = nullptr;
    int64_t off = eval_reloc(addr, &label);
    return new_num(off, ty_ulong, start);
  }

  if (tok_equal(tok, "__builtin_unreachable") || tok_equal(tok, "__builtin_trap")) {
    *rest = tok_skip(tok_skip(tok->next, "("), ")");
    Node *n = new_node(ND_UNREACHABLE, start);
    n->ty = ty_void;
    return n;
  }

  if (tok_equal(tok, "__builtin_expect")) {
    tok = tok_skip(tok->next, "(");
    Node *e = rvalue(assign(&tok, tok));
    tok = tok_skip(tok, ",");
    assign(&tok, tok);
    *rest = tok_skip(tok, ")");
    return implicit_cast(e, ty_long);
  }

  if (tok_equal(tok, "__builtin_inff") || tok_equal(tok, "__builtin_inf") ||
      tok_equal(tok, "__builtin_huge_valf") || tok_equal(tok, "__builtin_huge_val")) {
    bool is_float = tok->loc[tok->len - 1] == 'f';
    *rest = tok_skip(tok_skip(tok->next, "("), ")");
    return new_fnum(__builtin_inf(), is_float ? ty_float : ty_double, start);
  }

  if (tok_equal(tok, "__builtin_nanf") || tok_equal(tok, "__builtin_nan")) {
    bool is_float = tok->loc[tok->len - 1] == 'f';
    tok = tok_skip(tok->next, "(");
    assign(&tok, tok);
    *rest = tok_skip(tok, ")");
    return new_fnum(__builtin_nan(""), is_float ? ty_float : ty_double, start);
  }

  if (tok_equal(tok, "__builtin_constant_p")) {
    tok = tok_skip(tok->next, "(");
    P.unevaluated++;
    Node *e = assign(&tok, tok);
    P.unevaluated--;
    *rest = tok_skip(tok, ")");
    return new_num(is_scalar(e->ty) && is_const_expr(e), ty_int, start);
  }

  if (tok_equal(tok, "__builtin_types_compatible_p")) {
    tok = tok_skip(tok->next, "(");
    Type *a = typename_(&tok, tok);
    tok = tok_skip(tok, ",");
    Type *b = typename_(&tok, tok);
    *rest = tok_skip(tok, ")");
    return new_num(types_compatible(unqualified(a), unqualified(b)), ty_int, start);
  }

  if (tok_equal(tok, "__builtin_add_overflow") || tok_equal(tok, "__builtin_sub_overflow") ||
      tok_equal(tok, "__builtin_mul_overflow")) {
    NodeKind op = tok->loc[10] == 'a' ? ND_ADD : tok->loc[10] == 's' ? ND_SUB : ND_MUL;
    tok = tok_skip(tok->next, "(");
    Node *a = rvalue(assign(&tok, tok));
    tok = tok_skip(tok, ",");
    Node *b = rvalue(assign(&tok, tok));
    tok = tok_skip(tok, ",");
    Token *res_tok = tok;
    Node *res = rvalue(assign(&tok, tok));
    *rest = tok_skip(tok, ")");
    if (!is_integer(a->ty) || !is_integer(b->ty))
      error_tok(start, "operands of '%.*s' must be integers", start->len, start->loc);
    if (res->ty->kind != TY_PTR || !is_integer(res->ty->base) || res->ty->base->kind == TY_BOOL ||
        res->ty->base->kind == TY_ENUM || res->ty->base->is_const)
      error_tok(res_tok, "result argument must be a pointer to a modifiable integer");
    // The result is exact (infinite precision): each operand is widened to
    // 64 bits without changing its value, keeping its own signedness.
    Node *wa = implicit_cast(a, integer_promote(a->ty)->is_unsigned ? ty_ulong : ty_long);
    Node *wb = implicit_cast(b, integer_promote(b->ty)->is_unsigned ? ty_ulong : ty_long);
    Node *n = new_binary(ND_OVERFLOW, wa, wb, start);
    n->cond = res;
    n->val = op;
    n->ty = ty_bool;
    return n;
  }

  *handled = false;
  return nullptr;
}

static Node *identifier(Token **rest, Token *tok) {
  VarScope *vs = find_var(tok);
  if (vs && vs->var) {
    Obj *var = vs->var;
    var->refs++;
    var->is_referenced = true;
    if (!P.unevaluated) {
      var->weight += 1u << MIN(3 * P.loop_depth, 24);
      if (var->is_local && !var->has_init && var->writes == 0 && !var->first_read)
        var->first_read = tok;
    }
    if (var->is_deprecated)
      warn_tok(W_DEPRECATED, tok, "'%s' is deprecated%s%s", var->name, var->deprecated_msg ? ": " : "",
               var->deprecated_msg ? var->deprecated_msg : "");
    *rest = tok->next;
    return new_var_node(var, tok);
  }
  if (vs && vs->enum_ty) {
    *rest = tok->next;
    Type *ety = vs->enum_ty;
    // C23: enumeration constants have type int when the value fits.
    Type *ty = ety->is_fixed_enum ? ety
               : (vs->enum_val >= INT32_MIN && vs->enum_val <= INT32_MAX) ? ty_int
                                                                          : ety->base;
    return new_num(vs->enum_val, ty, tok);
  }
  if (vs && vs->type_def)
    error_tok(tok, "unexpected type name '%.*s': expected expression", tok->len, tok->loc);
  if (tok_equal(tok->next, "(")) {
    if (tok_equal(tok, "gets"))
      error_tok(tok, "'gets' was removed in C11 because it cannot be used safely; use fgets()");
    error_tok(tok, "call to undeclared function '%.*s'; ISO C99 and later do not support implicit function "
                   "declarations",
              tok->len, tok->loc);
  }
  error_tok(tok, "use of undeclared identifier '%.*s'", tok->len, tok->loc);
}

static Node *primary(Token **rest, Token *tok) {
  Token *start = tok;

  if (tok_equal(tok, "(")) {
    if (tok_equal(tok->next, "{"))
      error_tok(tok, "statement expressions are a GNU extension not supported by occ");
    Node *node = expr(&tok, tok->next);
    node->in_parens = true;
    *rest = tok_skip(tok, ")");
    return node;
  }

  if (tok->kind == TK_NUM) {
    Type *ty = literal_type(tok->lit);
    *rest = tok->next;
    if (is_flonum(ty))
      return new_fnum(tok->fval, ty, tok);
    return new_num(tok->ival, ty, tok);
  }

  if (tok->kind == TK_STR) {
    *rest = tok->next;
    return new_string_literal(tok);
  }

  if (tok_equal(tok, "true") || tok_equal(tok, "false")) {
    *rest = tok->next;
    return new_num(tok_equal(tok, "true"), ty_bool, tok);
  }

  if (tok_equal(tok, "nullptr")) {
    *rest = tok->next;
    return new_num(0, ty_nullptr, tok);
  }

  if (tok_equal(tok, "_Generic"))
    return generic_selection(rest, tok);

  if (tok->kind == TK_IDENT) {
    if (tok_equal(tok, "__func__") || tok_equal(tok, "__FUNCTION__")) {
      *rest = tok->next;
      return func_name_var(tok);
    }
    if (starts_with(tok->loc, "__builtin_")) {
      bool handled;
      Node *n = builtin_call(rest, tok, &handled);
      if (handled)
        return n;
    }
    return identifier(rest, tok);
  }

  if (tok->kind == TK_EOF)
    error_tok(start, "expected expression before end of file");
  error_tok(start, "expected expression");
}

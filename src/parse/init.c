// init.c - initializers (C23 6.7.11).
//
// An initializer is first parsed into a tree mirroring the object's type,
// honouring designators and brace elision. The tree is then lowered either
// to assignments (automatic storage) or to bytes and relocations (static
// storage).
#include "parse/parser.h"

typedef struct Initializer Initializer;
struct Initializer {
  Type *ty;
  bool is_flexible;        // array of unknown size, completed by this initializer
  Node *expr;              // value for scalars (or a whole struct/union)
  Initializer **children;  // array elements or struct/union members (by Member.idx)
  Member *mem;             // union: the member being initialized
};

// Access path from the variable to the subobject being initialized.
typedef struct InitDesg InitDesg;
struct InitDesg {
  InitDesg *next;
  int idx;
  Member *member;
  Obj *var;
};

static void initializer2(Token **rest, Token *tok, Initializer *init);

static int count_members(const Type *ty) {
  int n = 0;
  for (Member *m = ty->members; m; m = m->next)
    n++;
  return n;
}

static Initializer *new_initializer(Type *ty, bool is_flexible) {
  Initializer *init = NEW(Initializer);
  init->ty = ty;

  if (ty->kind == TY_ARRAY) {
    if (is_flexible && ty->array_len < 0) {
      init->is_flexible = true;
      return init;
    }
    int len = MAX(ty->array_len, 0);
    init->children = xcalloc((size_t)len, sizeof(Initializer *));
    for (int i = 0; i < len; i++)
      init->children[i] = new_initializer(ty->base, false);
    return init;
  }

  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    init->children = xcalloc((size_t)count_members(ty), sizeof(Initializer *));
    for (Member *m = ty->members; m; m = m->next) {
      if (is_flexible && ty->is_flexible && !m->next) {
        Initializer *child = NEW(Initializer);
        child->ty = m->ty;
        child->is_flexible = true;
        init->children[m->idx] = child;
      } else {
        init->children[m->idx] = new_initializer(m->ty, false);
      }
    }
  }
  return init;
}

// Unnamed bit-fields do not take part in initialization.
static Member *next_initializable(Member *m) {
  while (m && m->is_bitfield && !m->name)
    m = m->next;
  return m;
}

static bool is_end(Token *tok) {
  return tok_equal(tok, "}") || (tok_equal(tok, ",") && tok_equal(tok->next, "}"));
}

static bool consume_end(Token **rest, Token *tok) {
  if (tok_equal(tok, "}")) {
    *rest = tok->next;
    return true;
  }
  if (tok_equal(tok, ",") && tok_equal(tok->next, "}")) {
    *rest = tok->next->next;
    return true;
  }
  return false;
}

static Token *skip_excess_element(Token *tok) {
  if (tok_equal(tok, "{")) {
    tok = skip_excess_element(tok->next);
    while (!consume_end(&tok, tok)) {
      tok = tok_skip(tok, ",");
      tok = skip_excess_element(tok);
    }
    return tok;
  }
  assign(&tok, tok);
  return tok;
}

static void warn_excess(Token *tok) {
  warn_tok(W_OVERFLOW, tok, "excess elements in initializer");
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

static bool is_string_init(const Initializer *init, const Token *tok) {
  if (init->ty->kind != TY_ARRAY || tok->kind != TK_STR)
    return false;
  const Type *base = init->ty->base;
  return is_integer(base) && base->kind != TY_BOOL && base->size == encoding_elem_size(tok->enc);
}

static uint64_t read_unit(const char *p, int size) {
  uint64_t v = 0;
  for (int i = size - 1; i >= 0; i--)
    v = (v << 8) | (unsigned char)p[i];
  return v;
}

static void string_initializer(Token **rest, Token *tok, Initializer *init) {
  if (init->is_flexible)
    *init = *new_initializer(array_of(init->ty->base, tok->str_len), false);

  Type *base = init->ty->base;
  int n = MIN(init->ty->array_len, tok->str_len);
  if (tok->str_len - 1 > init->ty->array_len)
    warn_tok(W_OVERFLOW, tok, "initializer-string for array of '%s' is too long", type_name(base));
  for (int i = 0; i < n; i++) {
    uint64_t v = read_unit(tok->str + (size_t)i * (size_t)base->size, base->size);
    int64_t val = base->is_unsigned ? (int64_t)v
                  : base->size == 1 ? (int8_t)v
                  : base->size == 2 ? (int16_t)v
                                    : (int32_t)v;
    init->children[i]->expr = new_num(val, base, tok);
  }
  *rest = tok->next;
}

// ---------------------------------------------------------------------------
// Designators
// ---------------------------------------------------------------------------

static void array_designator(Token **rest, Token *tok, Type *ty, int *begin, int *end) {
  Token *start = tok->next;
  *begin = (int)const_expr(&tok, tok->next);
  if (*begin < 0 || (ty->array_len >= 0 && *begin >= ty->array_len))
    error_tok(start, "array designator index (%d) exceeds array bounds", *begin);
  *end = *begin;
  if (tok_equal(tok, "...")) { // GNU range designator [a ... b]
    *end = (int)const_expr(&tok, tok->next);
    if (*end < *begin || (ty->array_len >= 0 && *end >= ty->array_len))
      error_tok(start, "array designator range is invalid");
  }
  *rest = tok_skip(tok, "]");
}

// Finds the member named by ".name". A member inside an anonymous struct or
// union returns the anonymous member and leaves ".name" to be parsed again.
static Member *struct_designator(Token **rest, Token *tok, Type *ty) {
  Token *start = tok;
  tok = tok_skip(tok, ".");
  if (tok->kind != TK_IDENT)
    error_tok(tok, "expected a field designator");
  for (Member *m = ty->members; m; m = m->next) {
    if (!m->name && (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION)) {
      for (Member *sub = m->ty->members; sub; sub = sub->next) {
        if (sub->name && sub->name->len == tok->len && !memcmp(sub->name->loc, tok->loc, (size_t)tok->len)) {
          *rest = start;
          return m;
        }
      }
      continue;
    }
    if (m->name && m->name->len == tok->len && !memcmp(m->name->loc, tok->loc, (size_t)tok->len)) {
      *rest = tok->next;
      return m;
    }
  }
  error_tok(tok, "field designator '%.*s' does not refer to any field in type '%s'", tok->len, tok->loc,
            type_name(ty));
}

static void array_initializer2(Token **rest, Token *tok, Initializer *init, int i);
static void struct_initializer2(Token **rest, Token *tok, Initializer *init, Member *mem);

static void designation(Token **rest, Token *tok, Initializer *init) {
  if (tok_equal(tok, "[")) {
    if (init->ty->kind != TY_ARRAY)
      error_tok(tok, "array index in initializer of non-array type '%s'", type_name(init->ty));
    int begin, end;
    array_designator(&tok, tok, init->ty, &begin, &end);
    Token *tok2 = tok;
    for (int i = begin; i <= end; i++)
      designation(&tok2, tok, init->children[i]);
    array_initializer2(rest, tok2, init, begin + 1);
    return;
  }

  if (tok_equal(tok, ".") && init->ty->kind == TY_STRUCT) {
    Member *mem = struct_designator(&tok, tok, init->ty);
    designation(&tok, tok, init->children[mem->idx]);
    init->expr = nullptr;
    struct_initializer2(rest, tok, init, next_initializable(mem->next));
    return;
  }

  if (tok_equal(tok, ".") && init->ty->kind == TY_UNION) {
    Member *mem = struct_designator(&tok, tok, init->ty);
    init->mem = mem;
    designation(rest, tok, init->children[mem->idx]);
    return;
  }

  if (tok_equal(tok, "."))
    error_tok(tok, "field designator used on non-struct type '%s'", type_name(init->ty));

  if (tok_equal(tok, "="))
    tok = tok->next;
  initializer2(rest, tok, init);
}

// ---------------------------------------------------------------------------
// Arrays, structs and unions
// ---------------------------------------------------------------------------

static int count_array_init_elements(Token *tok, Type *ty) {
  Initializer *dummy = new_initializer(ty->base, true);
  int i = 0, max = 0;
  bool first = true;
  while (!consume_end(&tok, tok)) {
    if (!first)
      tok = tok_skip(tok, ",");
    first = false;
    if (tok_equal(tok, "[")) {
      Token *start = tok->next;
      i = (int)const_expr(&tok, tok->next);
      if (i < 0)
        error_tok(start, "array designator index is negative");
      if (tok_equal(tok, "..."))
        i = (int)const_expr(&tok, tok->next);
      tok = tok_skip(tok, "]");
      designation(&tok, tok, dummy);
    } else {
      initializer2(&tok, tok, dummy);
    }
    i++;
    max = MAX(max, i);
  }
  return max;
}

// { e0, e1, [4] = e4, ... }
static void array_initializer1(Token **rest, Token *tok, Initializer *init) {
  tok = tok_skip(tok, "{");
  if (init->is_flexible) {
    int len = count_array_init_elements(tok, init->ty);
    *init = *new_initializer(array_of(init->ty->base, len), false);
  }

  bool first = true;
  for (int i = 0; !consume_end(rest, tok); i++) {
    if (!first)
      tok = tok_skip(tok, ",");
    first = false;

    if (tok_equal(tok, "[")) {
      int begin, end;
      array_designator(&tok, tok, init->ty, &begin, &end);
      Token *tok2 = tok;
      for (int j = begin; j <= end; j++)
        designation(&tok2, tok, init->children[j]);
      tok = tok2;
      i = end;
      continue;
    }

    if (i < init->ty->array_len) {
      initializer2(&tok, tok, init->children[i]);
    } else {
      warn_excess(tok);
      tok = skip_excess_element(tok);
    }
  }
}

// Brace-elided array: consumes as many elements as the array holds.
static void array_initializer2(Token **rest, Token *tok, Initializer *init, int i) {
  if (init->is_flexible) {
    int len = count_array_init_elements(tok, init->ty);
    *init = *new_initializer(array_of(init->ty->base, len), false);
  }
  for (bool first = i == 0; i < init->ty->array_len && !is_end(tok); i++) {
    Token *start = tok;
    if (!first)
      tok = tok_skip(tok, ",");
    first = false;
    if (tok_equal(tok, "[") || tok_equal(tok, ".")) {
      *rest = start;
      return;
    }
    initializer2(&tok, tok, init->children[i]);
  }
  *rest = tok;
}

static void struct_initializer1(Token **rest, Token *tok, Initializer *init) {
  tok = tok_skip(tok, "{");
  Member *mem = next_initializable(init->ty->members);
  bool first = true;

  while (!consume_end(rest, tok)) {
    if (!first)
      tok = tok_skip(tok, ",");
    first = false;

    if (tok_equal(tok, ".")) {
      mem = struct_designator(&tok, tok, init->ty);
      designation(&tok, tok, init->children[mem->idx]);
      mem = next_initializable(mem->next);
      continue;
    }

    if (mem) {
      initializer2(&tok, tok, init->children[mem->idx]);
      mem = next_initializable(mem->next);
    } else {
      warn_excess(tok);
      tok = skip_excess_element(tok);
    }
  }
}

static void struct_initializer2(Token **rest, Token *tok, Initializer *init, Member *mem) {
  bool first = true;
  for (; mem && !is_end(tok); mem = next_initializable(mem->next)) {
    Token *start = tok;
    if (!first)
      tok = tok_skip(tok, ",");
    first = false;
    if (tok_equal(tok, "[") || tok_equal(tok, ".")) {
      *rest = start;
      return;
    }
    initializer2(&tok, tok, init->children[mem->idx]);
  }
  *rest = tok;
}

static void union_initializer(Token **rest, Token *tok, Initializer *init) {
  // A designator can select any member; otherwise the first named one.
  if (tok_equal(tok, "{") && tok_equal(tok->next, ".")) {
    Member *mem = struct_designator(&tok, tok->next, init->ty);
    init->mem = mem;
    designation(&tok, tok, init->children[mem->idx]);
    tok_consume(&tok, tok, ",");
    *rest = tok_skip(tok, "}");
    return;
  }

  Member *mem = next_initializable(init->ty->members);
  init->mem = mem;
  if (tok_equal(tok, "{")) {
    if (tok_equal(tok->next, "}")) {
      *rest = tok->next->next;
      return;
    }
    if (!mem)
      error_tok(tok, "initializer for an empty union");
    initializer2(&tok, tok->next, init->children[mem->idx]);
    while (tok_equal(tok, ",") && !tok_equal(tok->next, "}")) {
      warn_excess(tok->next);
      tok = skip_excess_element(tok->next);
    }
    tok_consume(&tok, tok, ",");
    *rest = tok_skip(tok, "}");
    return;
  }
  if (!mem)
    error_tok(tok, "initializer for an empty union");
  initializer2(rest, tok, init->children[mem->idx]);
}

static bool is_char_array(const Type *ty) {
  return ty->kind == TY_ARRAY && is_integer(ty->base) && ty->base->kind != TY_BOOL;
}

static void initializer2(Token **rest, Token *tok, Initializer *init) {
  if (is_string_init(init, tok)) {
    string_initializer(rest, tok, init);
    return;
  }

  if (init->ty->kind == TY_ARRAY) {
    if (tok_equal(tok, "{")) {
      // char s[] = { "abc" };
      if (is_char_array(init->ty) && is_string_init(init, tok->next) && is_end(tok->next->next)) {
        string_initializer(&tok, tok->next, init);
        tok_consume(&tok, tok, ",");
        *rest = tok_skip(tok, "}");
        return;
      }
      array_initializer1(rest, tok, init);
    } else {
      array_initializer2(rest, tok, init, 0);
    }
    return;
  }

  if (init->ty->kind == TY_STRUCT || init->ty->kind == TY_UNION) {
    if (!tok_equal(tok, "{")) {
      // An expression of compatible type initializes the whole aggregate;
      // anything else is the first element of a brace-elided list.
      Token *end;
      Node *e = assign(&end, tok);
      if (types_compatible(unqualified(e->ty), unqualified(init->ty))) {
        init->expr = e;
        *rest = end;
        return;
      }
    }
    if (init->ty->kind == TY_UNION)
      union_initializer(rest, tok, init);
    else if (tok_equal(tok, "{"))
      struct_initializer1(rest, tok, init);
    else
      struct_initializer2(rest, tok, init, next_initializable(init->ty->members));
    return;
  }

  if (tok_equal(tok, "{")) {
    // Braces around a scalar ("int x = {3};"), or C23 empty braces.
    if (tok_equal(tok->next, "}")) {
      init->expr = is_flonum(init->ty) ? new_node(ND_NUM, tok) : new_num(0, init->ty, tok);
      init->expr->ty = init->ty;
      *rest = tok->next->next;
      return;
    }
    initializer2(&tok, tok->next, init);
    while (tok_equal(tok, ",") && !tok_equal(tok->next, "}")) {
      warn_excess(tok->next);
      tok = skip_excess_element(tok->next);
    }
    tok_consume(&tok, tok, ",");
    *rest = tok_skip(tok, "}");
    return;
  }

  init->expr = assign(rest, tok);
}

// Completes a struct with an initialized flexible array member (GNU extension
// for objects with static storage).
static Type *complete_flexible_struct(Type *ty, Initializer *init) {
  Member *last = ty->members;
  while (last->next)
    last = last->next;
  Type *arr = init->children[last->idx]->ty;
  if (arr->array_len <= 0)
    return ty;

  Type *t = struct_type(ty->kind);
  *t = *ty;
  t->origin = nullptr;
  t->next_copy = nullptr;
  Member head = {};
  Member *cur = &head;
  for (Member *m = ty->members; m; m = m->next) {
    Member *copy = NEW(Member);
    *copy = *m;
    if (!m->next)
      copy->ty = arr;
    cur = cur->next = copy;
  }
  t->members = head.next;
  t->size = (int)align_to(last->offset + arr->size, t->align);
  return t;
}

static Initializer *parse_initializer(Token **rest, Token *tok, Type *ty, Type **new_ty) {
  Initializer *init = new_initializer(ty, true);
  initializer2(rest, tok, init);
  *new_ty = init->ty;
  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_flexible && !init->expr)
    *new_ty = complete_flexible_struct(ty, init);
  return init;
}

// ---------------------------------------------------------------------------
// Lowering: automatic storage
// ---------------------------------------------------------------------------

static Node *comma(Node *lhs, Node *rhs) {
  if (!lhs)
    return rhs;
  if (!rhs)
    return lhs;
  Node *n = new_binary(ND_COMMA, lhs, rhs, rhs->tok);
  n->ty = rhs->ty;
  return n;
}

static Node *init_desg_expr(InitDesg *desg, Token *tok) {
  if (desg->var)
    return new_var_node(desg->var, tok);
  if (desg->member) {
    Node *n = new_unary(ND_MEMBER, init_desg_expr(desg->next, tok), tok);
    n->member = desg->member;
    n->ty = desg->member->ty;
    return n;
  }
  Node *base = init_desg_expr(desg->next, tok);
  return new_deref(new_add(base, new_num(desg->idx, ty_long, tok), tok), tok);
}

// Builds the assignments for one subobject, returning them as a
// right-leaning comma chain (codegen walks such chains iteratively).
static void create_lvar_init(Initializer *init, Type *ty, InitDesg *desg, Token *tok, Node ***tail) {
  if (ty->kind == TY_ARRAY) {
    for (int i = 0; i < ty->array_len; i++) {
      InitDesg d = {.next = desg, .idx = i};
      create_lvar_init(init->children[i], ty->base, &d, tok, tail);
    }
    return;
  }

  if (ty->kind == TY_STRUCT && !init->expr) {
    for (Member *m = next_initializable(ty->members); m; m = next_initializable(m->next)) {
      InitDesg d = {.next = desg, .member = m};
      create_lvar_init(init->children[m->idx], m->ty, &d, tok, tail);
    }
    return;
  }

  if (ty->kind == TY_UNION && !init->expr) {
    Member *m = init->mem ? init->mem : next_initializable(ty->members);
    if (!m)
      return;
    InitDesg d = {.next = desg, .member = m};
    create_lvar_init(init->children[m->idx], m->ty, &d, tok, tail);
    return;
  }

  if (!init->expr)
    return;

  Node *lhs = init_desg_expr(desg, tok);
  Node *rhs = convert_for_assign(init->expr, ty, init->expr->tok, "initializing");
  Node *asg = new_binary(ND_ASSIGN, lhs, rhs, tok);
  asg->ty = unqualified(ty);

  // Append to the chain: each link is COMMA(assignment, rest).
  Node *link = new_binary(ND_COMMA, asg, nullptr, tok);
  **tail = link;
  *tail = &link->rhs;
}

static void capture_const_value(Obj *var, Node *value) {
  if (!var->is_constexpr || !is_scalar(var->ty))
    return;
  if (!value || !is_const_expr(value))
    error_tok(var->tok, "constexpr variable '%s' must be initialized by a constant expression", var->name);
  var->has_const_value = true;
  if (is_flonum(var->ty))
    var->const_fval = eval_double(value);
  else
    var->const_ival = eval_int(value);
}

Node *lvar_initializer(Token **rest, Token *tok, Obj *var) {
  Token *start = tok;
  Initializer *init = parse_initializer(rest, tok, var->ty, &var->ty);
  if (var->ty != init->ty && is_aggregate(var->ty) && var->ty->kind != TY_ARRAY)
    error_tok(start, "initialization of a flexible array member is not allowed here");
  InitDesg desg = {.var = var};

  Node *chain = nullptr;
  Node **tail = &chain;
  create_lvar_init(init, var->ty, &desg, start, &tail);

  Node *terminator = new_node(ND_NOP, start);
  terminator->ty = ty_void;
  *tail = terminator;
  // Fix the types of the comma links (type of the whole chain is void).
  for (Node *n = chain; n->kind == ND_COMMA; n = n->rhs)
    n->ty = ty_void;

  if (is_scalar(var->ty) && chain->kind == ND_COMMA)
    capture_const_value(var, chain->lhs->rhs);

  if (!is_aggregate(var->ty))
    return chain;
  // Aggregates are zeroed first: omitted members are zero-initialized.
  Node *zero = new_node(ND_MEMZERO, start);
  zero->var = var;
  zero->ty = ty_void;
  return comma(zero, chain);
}

// ---------------------------------------------------------------------------
// Lowering: static storage
// ---------------------------------------------------------------------------

static void write_buf(char *buf, uint64_t val, int size) {
  for (int i = 0; i < size; i++)
    buf[i] = (char)(val >> (8 * i));
}

static Reloc *write_gvar_data(Reloc *cur, Initializer *init, Type *ty, char *buf, int offset) {
  if (ty->kind == TY_ARRAY) {
    int sz = ty->base->size;
    for (int i = 0; i < ty->array_len; i++)
      cur = write_gvar_data(cur, init->children[i], ty->base, buf, offset + sz * i);
    return cur;
  }

  if (ty->kind == TY_STRUCT && !init->expr) {
    for (Member *m = next_initializable(ty->members); m; m = next_initializable(m->next)) {
      Initializer *child = init->children[m->idx];
      if (m->is_bitfield) {
        if (!child->expr)
          continue;
        char *loc = buf + offset + m->offset;
        uint64_t old = read_unit(loc, m->ty->size);
        Node *e = convert_for_assign(child->expr, m->ty, child->expr->tok, "initializing");
        uint64_t val = (uint64_t)eval_int(e);
        uint64_t mask = m->bit_width == 64 ? ~0ULL : (1ULL << m->bit_width) - 1;
        write_buf(loc, old | ((val & mask) << m->bit_offset), m->ty->size);
        continue;
      }
      cur = write_gvar_data(cur, child, m->ty, buf, offset + m->offset);
    }
    return cur;
  }

  if (ty->kind == TY_UNION && !init->expr) {
    Member *m = init->mem ? init->mem : next_initializable(ty->members);
    if (!m)
      return cur;
    return write_gvar_data(cur, init->children[m->idx], m->ty, buf, offset);
  }

  if (!init->expr)
    return cur;

  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    // Only a file-scope compound literal is a constant of aggregate type.
    Node *e = init->expr;
    while (e->kind == ND_COMMA)
      e = e->rhs;
    if (e->kind != ND_VAR || e->var->is_local || !e->var->init_data)
      error_tok(init->expr->tok, "initializer element is not a compile-time constant");
    memcpy(buf + offset, e->var->init_data, (size_t)ty->size);
    for (Reloc *r = e->var->rel; r; r = r->next) {
      Reloc *copy = NEW(Reloc);
      *copy = *r;
      copy->offset += offset;
      cur = cur->next = copy;
    }
    return cur;
  }

  Node *e = convert_for_assign(init->expr, ty, init->expr->tok, "initializing");
  init->expr = e;
  if (!is_const_expr(e))
    error_tok(e->tok, "initializer element is not a compile-time constant");

  if (is_flonum(ty)) {
    double v = eval_double(e);
    if (ty->kind == TY_FLOAT) {
      float f = (float)v;
      memcpy(buf + offset, &f, 4);
    } else {
      memcpy(buf + offset, &v, 8);
    }
    return cur;
  }

  char *label = nullptr;
  int64_t val = eval_reloc(e, &label);
  if (!label) {
    write_buf(buf + offset, (uint64_t)val, ty->size);
    return cur;
  }
  if (ty->size != 8)
    error_tok(e->tok, "initializer element is not computable at load time");
  Reloc *rel = NEW(Reloc);
  rel->offset = offset;
  rel->label = label;
  rel->addend = val;
  cur->next = rel;
  return rel;
}

void gvar_initializer(Token **rest, Token *tok, Obj *var) {
  Initializer *init = parse_initializer(rest, tok, var->ty, &var->ty);
  var->align = MAX(var->align, var->ty->align);
  Reloc head = {};
  char *buf = xcalloc(1, (size_t)MAX(var->ty->size, 1));
  write_gvar_data(&head, init, var->ty, buf, 0);
  var->init_data = buf;
  var->rel = head.next;
  if (is_scalar(var->ty))
    capture_const_value(var, init->expr);
}

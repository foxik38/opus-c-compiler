// scope.c - block scopes, symbol lookup and object creation.
#include "parse/parser.h"

ParserState P;

static Scope file_scope;
static Scope *scope = &file_scope;
static int unique_counter;
static int label_counter;

void scope_reset(void) {
  file_scope = (Scope){};
  scope = &file_scope;
}

void enter_scope(void) {
  Scope *sc = NEW(Scope);
  sc->next = scope;
  scope = sc;
}

void leave_scope(void) {
  check_unused_locals(scope);
  scope = scope->next;
}

bool at_file_scope(void) { return scope == &file_scope; }

VarScope *push_scope(char *name) {
  VarScope *vs = NEW(VarScope);
  hashmap_put(&scope->vars, name, vs);
  return vs;
}

VarScope *find_var(const Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next) {
    VarScope *vs = hashmap_get2(&sc->vars, tok->loc, tok->len);
    if (vs)
      return vs;
  }
  return nullptr;
}

VarScope *find_var_in_current_scope(const Token *tok) {
  return hashmap_get2(&scope->vars, tok->loc, tok->len);
}

Type *find_tag(const Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next) {
    Type *ty = hashmap_get2(&sc->tags, tok->loc, tok->len);
    if (ty)
      return ty;
  }
  return nullptr;
}

Type *find_tag_in_current_scope(const Token *tok) {
  return hashmap_get2(&scope->tags, tok->loc, tok->len);
}

void push_tag(const Token *tok, Type *ty) { hashmap_put2(&scope->tags, tok->loc, tok->len, ty); }

static Obj *new_var(char *name, Type *ty) {
  Obj *var = NEW(Obj);
  var->name = name;
  var->ty = ty;
  var->align = ty->align;
  var->reg = -1;
  push_scope(name)->var = var;
  return var;
}

Obj *new_lvar(char *name, Type *ty, Token *tok) {
  Obj *var = new_var(name, ty);
  var->is_local = true;
  var->tok = tok;
  if (P.current_fn) {
    var->next = P.current_fn->locals;
    P.current_fn->locals = var;
  }
  if (tok)
    vec_push(&scope->locals, var);
  return var;
}

Obj *new_temp(Type *ty) {
  Obj *var = NEW(Obj);
  var->name = "";
  var->ty = ty;
  var->align = ty->align;
  var->reg = -1;
  var->is_local = true;
  // Temporaries are used a few times right where they are created.
  var->weight = 2u << MIN(3 * P.loop_depth, 24);
  var->next = P.current_fn->locals;
  P.current_fn->locals = var;
  return var;
}

static void append_global(Obj *var) {
  // Keep definition order: codegen emits globals in source order.
  static Obj *last;
  if (!P.globals)
    last = nullptr;
  if (last)
    last->next = var;
  else
    P.globals = var;
  last = var;
}

Obj *new_gvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->is_definition = true;
  append_global(var);
  return var;
}

char *unique_name(const char *prefix) { return format(".L.%s.%d", prefix, unique_counter++); }

int new_label(void) { return ++label_counter; }

Obj *new_anon_gvar(Type *ty) {
  Obj *var = NEW(Obj);
  var->name = unique_name("anon");
  var->ty = ty;
  var->align = ty->align;
  var->reg = -1;
  var->is_definition = true;
  var->is_static = true;
  append_global(var);
  return var;
}

Node *new_string_literal(Token *tok) {
  static Type *const *elem_types[] = {
      [ENC_CHAR] = &ty_char, [ENC_UTF8] = &ty_uchar, [ENC_UTF16] = &ty_ushort,
      [ENC_UTF32] = &ty_uint, [ENC_WIDE] = &ty_int,
  };
  Type *ty = array_of(*elem_types[tok->enc], tok->str_len);
  Obj *var = new_anon_gvar(ty);
  var->name = unique_name("str");
  var->init_data = tok->str;
  var->is_string_literal = true;
  return new_var_node(var, tok);
}

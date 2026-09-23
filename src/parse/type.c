// type.c - C type construction, classification and conversions (LP64).
#include "parse/ast.h"
#include "support/strbuf.h"

#define BASIC(k, sz, uns) (&(Type){.kind = (k), .is_unsigned = (uns), .size = (sz), .align = (sz)})

Type *ty_void = BASIC(TY_VOID, 1, false);
Type *ty_bool = BASIC(TY_BOOL, 1, true);
Type *ty_nullptr = BASIC(TY_NULLPTR, 8, false);

Type *ty_char = BASIC(TY_CHAR, 1, false); // plain char is signed on x86-64
Type *ty_schar = &(Type){.kind = TY_CHAR, .is_signed_char = true, .size = 1, .align = 1};
Type *ty_short = BASIC(TY_SHORT, 2, false);
Type *ty_int = BASIC(TY_INT, 4, false);
Type *ty_long = BASIC(TY_LONG, 8, false);
Type *ty_llong = BASIC(TY_LLONG, 8, false);

Type *ty_uchar = BASIC(TY_CHAR, 1, true);
Type *ty_ushort = BASIC(TY_SHORT, 2, true);
Type *ty_uint = BASIC(TY_INT, 4, true);
Type *ty_ulong = BASIC(TY_LONG, 8, true);
Type *ty_ullong = BASIC(TY_LLONG, 8, true);

Type *ty_float = BASIC(TY_FLOAT, 4, false);
Type *ty_double = BASIC(TY_DOUBLE, 8, false);
Type *ty_ldouble = BASIC(TY_LDOUBLE, 8, false);

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

bool is_integer(const Type *ty) {
  switch (ty->kind) {
  case TY_BOOL: case TY_CHAR: case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG: case TY_ENUM:
    return true;
  default:
    return false;
  }
}

bool is_signed_integer(const Type *ty) { return is_integer(ty) && !ty->is_unsigned; }

bool is_flonum(const Type *ty) {
  return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE;
}

bool is_numeric(const Type *ty) { return is_integer(ty) || is_flonum(ty); }

bool is_pointer_like(const Type *ty) { return ty->kind == TY_PTR || ty->kind == TY_NULLPTR; }

bool is_scalar(const Type *ty) { return is_numeric(ty) || is_pointer_like(ty); }

bool is_aggregate(const Type *ty) {
  return ty->kind == TY_STRUCT || ty->kind == TY_UNION || ty->kind == TY_ARRAY;
}

bool is_complete(const Type *ty) {
  if (ty->kind == TY_VOID || ty->kind == TY_FUNC)
    return false;
  if (ty->kind == TY_ARRAY)
    return ty->array_len >= 0 && is_complete(ty->base);
  return ty->size >= 0;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Type *copy_type(Type *ty) {
  Type *t = NEW(Type);
  *t = *ty;
  t->origin = ty->origin ? ty->origin : ty;
  t->next = nullptr;
  t->next_copy = nullptr;
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    // Register the copy so that completing the struct later updates it too.
    Type *canon = t->origin;
    t->next_copy = canon->next_copy;
    canon->next_copy = t;
  }
  return t;
}

Type *pointer_to(Type *base) {
  Type *ty = NEW(Type);
  ty->kind = TY_PTR;
  ty->size = 8;
  ty->align = 8;
  ty->is_unsigned = true;
  ty->base = base;
  return ty;
}

Type *array_of(Type *base, int len) {
  Type *ty = NEW(Type);
  ty->kind = TY_ARRAY;
  ty->base = base;
  ty->array_len = len;
  ty->size = len >= 0 && base->size >= 0 ? base->size * len : -1;
  ty->align = base->align;
  return ty;
}

Type *func_type(Type *return_ty) {
  Type *ty = NEW(Type);
  ty->kind = TY_FUNC;
  ty->size = 1;
  ty->align = 1;
  ty->return_ty = return_ty;
  return ty;
}

Type *enum_type(void) {
  Type *ty = NEW(Type);
  ty->kind = TY_ENUM;
  ty->size = 4;
  ty->align = 4;
  ty->base = ty_int;
  return ty;
}

Type *struct_type(TypeKind kind) {
  Type *ty = NEW(Type);
  ty->kind = kind;
  ty->size = -1;
  ty->align = 1;
  return ty;
}

Type *qualified(Type *ty, bool is_const, bool is_volatile) {
  if ((!is_const || ty->is_const) && (!is_volatile || ty->is_volatile))
    return ty;
  // Qualifiers on an array type apply to its elements (C23 6.7.4.1).
  if (ty->kind == TY_ARRAY) {
    Type *t = array_of(qualified(ty->base, is_const, is_volatile), ty->array_len);
    t->name = ty->name;
    t->name_pos = ty->name_pos;
    return t;
  }
  Type *t = copy_type(ty);
  t->is_const |= is_const;
  t->is_volatile |= is_volatile;
  return t;
}

Type *unqualified(Type *ty) {
  if (!ty->is_const && !ty->is_volatile && !ty->is_restrict && !ty->is_atomic)
    return ty;
  if (ty->kind == TY_ARRAY)
    return ty;
  Type *t = copy_type(ty);
  t->is_const = t->is_volatile = t->is_restrict = t->is_atomic = false;
  return t;
}

void complete_struct(Type *ty) {
  Type *canon = ty->origin ? ty->origin : ty;
  for (Type *c = canon->next_copy; c; c = c->next_copy) {
    c->members = canon->members;
    c->size = canon->size;
    c->align = canon->align;
    c->is_flexible = canon->is_flexible;
    c->is_packed = canon->is_packed;
  }
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

Type *enum_underlying(Type *ty) { return ty->kind == TY_ENUM ? ty->base : ty; }

int integer_rank(const Type *ty) {
  switch (ty->kind) {
  case TY_BOOL: return 0;
  case TY_CHAR: return 1;
  case TY_SHORT: return 2;
  case TY_INT: return 3;
  case TY_LONG: return 4;
  case TY_LLONG: return 5;
  case TY_ENUM: return integer_rank(ty->base);
  default: return 3;
  }
}

static Type *unqualified_basic(Type *ty) {
  switch (ty->kind) {
  case TY_BOOL: return ty_bool;
  case TY_CHAR: return ty->is_unsigned ? ty_uchar : ty->is_signed_char ? ty_schar : ty_char;
  case TY_SHORT: return ty->is_unsigned ? ty_ushort : ty_short;
  case TY_INT: return ty->is_unsigned ? ty_uint : ty_int;
  case TY_LONG: return ty->is_unsigned ? ty_ulong : ty_long;
  case TY_LLONG: return ty->is_unsigned ? ty_ullong : ty_llong;
  case TY_FLOAT: return ty_float;
  case TY_DOUBLE: return ty_double;
  case TY_LDOUBLE: return ty_ldouble;
  default: return ty;
  }
}

Type *integer_promote(Type *ty) {
  ty = enum_underlying(ty);
  if (is_integer(ty) && ty->size < 4)
    return ty_int; // every value of char/short/bool fits in int
  return unqualified_basic(ty);
}

Type *common_type(Type *a, Type *b) {
  if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE)
    return ty_ldouble;
  if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE)
    return ty_double;
  if (a->kind == TY_FLOAT || b->kind == TY_FLOAT)
    return ty_float;

  a = integer_promote(a);
  b = integer_promote(b);
  if (a->size != b->size)
    return a->size > b->size ? a : b;
  // Same size: unsigned wins; between long and long long pick the higher rank.
  Type *r = integer_rank(a) >= integer_rank(b) ? a : b;
  if (a->is_unsigned || b->is_unsigned)
    return r->kind == TY_LLONG ? ty_ullong : r->kind == TY_LONG ? ty_ulong : ty_uint;
  return r;
}

// ---------------------------------------------------------------------------
// Compatibility (C23 6.2.7)
// ---------------------------------------------------------------------------

bool types_compatible(Type *a, Type *b) {
  if (a == b)
    return true;
  if (a->origin)
    return types_compatible(a->origin, b);
  if (b->origin)
    return types_compatible(a, b->origin);

  // An enum is compatible with its underlying integer type.
  if (a->kind == TY_ENUM && b->kind != TY_ENUM)
    return types_compatible(a->base, b);
  if (b->kind == TY_ENUM && a->kind != TY_ENUM)
    return types_compatible(a, b->base);

  if (a->kind != b->kind)
    return false;

  switch (a->kind) {
  case TY_CHAR:
    return a->is_unsigned == b->is_unsigned && a->is_signed_char == b->is_signed_char;
  case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG:
    return a->is_unsigned == b->is_unsigned;
  case TY_BOOL: case TY_FLOAT: case TY_DOUBLE: case TY_LDOUBLE: case TY_VOID: case TY_NULLPTR:
    return true;
  case TY_PTR:
    return a->base->is_const == b->base->is_const && a->base->is_volatile == b->base->is_volatile &&
           types_compatible(a->base, b->base);
  case TY_ARRAY:
    if (!types_compatible(a->base, b->base))
      return false;
    return a->array_len < 0 || b->array_len < 0 || a->array_len == b->array_len;
  case TY_FUNC: {
    if (!types_compatible(a->return_ty, b->return_ty) || a->is_variadic != b->is_variadic)
      return false;
    Type *p = a->params, *q = b->params;
    for (; p && q; p = p->next, q = q->next)
      if (!types_compatible(unqualified(p), unqualified(q)))
        return false;
    return !p && !q;
  }
  default:
    return false; // distinct struct/union/enum types
  }
}

// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------

static void print_qualifiers(StrBuf *sb, const Type *ty) {
  if (ty->is_const)
    sb_puts(sb, "const ");
  if (ty->is_volatile)
    sb_puts(sb, "volatile ");
}

static const char *basic_name(const Type *ty) {
  static const char *names[][2] = {
      [TY_VOID] = {"void", "void"},
      [TY_BOOL] = {"bool", "bool"},
      [TY_CHAR] = {"char", "unsigned char"},
      [TY_SHORT] = {"short", "unsigned short"},
      [TY_INT] = {"int", "unsigned int"},
      [TY_LONG] = {"long", "unsigned long"},
      [TY_LLONG] = {"long long", "unsigned long long"},
      [TY_FLOAT] = {"float", "float"},
      [TY_DOUBLE] = {"double", "double"},
      [TY_LDOUBLE] = {"long double", "long double"},
      [TY_NULLPTR] = {"nullptr_t", "nullptr_t"},
  };
  if (ty->is_signed_char)
    return "signed char";
  return names[ty->kind][ty->kind == TY_BOOL ? 0 : ty->is_unsigned];
}

// Prints a type in declarator form, e.g. "int (*)(char *)".
static void print_type(StrBuf *sb, const Type *ty, const char *inner) {
  switch (ty->kind) {
  case TY_PTR: {
    const Type *b = ty->base;
    bool paren = b->kind == TY_ARRAY || b->kind == TY_FUNC;
    char *s = format("%s*%s%s%s", paren ? "(" : "", ty->is_const ? " const" : "", inner, paren ? ")" : "");
    print_type(sb, b, s);
    return;
  }
  case TY_ARRAY: {
    char *s = ty->array_len >= 0 ? format("%s[%d]", inner, ty->array_len) : format("%s[]", inner);
    print_type(sb, ty->base, s);
    return;
  }
  case TY_FUNC: {
    StrBuf params = {};
    sb_printf(&params, "%s(", inner);
    for (const Type *p = ty->params; p; p = p->next) {
      if (p != ty->params)
        sb_puts(&params, ", ");
      sb_puts(&params, type_name(p));
    }
    if (ty->is_variadic)
      sb_puts(&params, ty->params ? ", ..." : "...");
    else if (!ty->params)
      sb_puts(&params, "void");
    sb_putc(&params, ')');
    print_type(sb, ty->return_ty, params.data);
    return;
  }
  case TY_STRUCT:
  case TY_UNION:
  case TY_ENUM:
    print_qualifiers(sb, ty);
    sb_puts(sb, ty->kind == TY_STRUCT ? "struct " : ty->kind == TY_UNION ? "union " : "enum ");
    if (ty->tag)
      sb_append(sb, ty->tag->loc, (size_t)ty->tag->len);
    else
      sb_puts(sb, "<anonymous>");
    break;
  default:
    print_qualifiers(sb, ty);
    sb_puts(sb, basic_name(ty));
    break;
  }
  if (*inner) {
    if (inner[0] != '[' && inner[0] != '(')
      sb_putc(sb, ' ');
    sb_puts(sb, inner);
  }
}

char *type_name(const Type *ty) {
  StrBuf sb = {};
  print_type(&sb, ty, "");
  return sb.data;
}

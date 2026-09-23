// decl.c - declarations: specifiers, declarators, tags, typedefs and functions.
#include "parse/parser.h"

// ---------------------------------------------------------------------------
// Attributes: [[...]] (C23) and __attribute__((...)) (GNU)
// ---------------------------------------------------------------------------

static Token *skip_balanced_parens(Token *tok) {
  Token *start = tok;
  int depth = 0;
  do {
    if (tok->kind == TK_EOF)
      error_tok(start, "unbalanced parentheses in attribute");
    if (tok_equal(tok, "("))
      depth++;
    else if (tok_equal(tok, ")"))
      depth--;
    tok = tok->next;
  } while (depth > 0);
  return tok;
}

// Optional ("message") argument of deprecated/nodiscard.
static char *attribute_message(Token **rest, Token *tok) {
  if (!tok_equal(tok, "(")) {
    *rest = tok;
    return nullptr;
  }
  tok = tok->next;
  if (tok->kind != TK_STR)
    error_tok(tok, "expected a string literal as the attribute argument");
  char *msg = tok->str;
  *rest = tok_skip(tok->next, ")");
  return msg;
}

// Strips the reserved __name__ spelling of an attribute name.
static char *attribute_name(Token *tok) {
  char *name = tok_text(tok);
  size_t n = strlen(name);
  if (n > 4 && starts_with(name, "__") && ends_with(name, "__"))
    return xstrndup(name + 2, n - 4);
  return name;
}

// Applies one attribute; tok points after its name. Returns the token after
// its (optional) argument list.
static Token *apply_attribute(Token *name_tok, Token *tok, Attributes *attrs, bool vendor) {
  char *name = attribute_name(name_tok);

  if (strcmp(name, "deprecated") == 0) {
    attrs->deprecated = true;
    attrs->deprecated_msg = attribute_message(&tok, tok);
    return tok;
  }
  if (strcmp(name, "nodiscard") == 0 || (vendor && strcmp(name, "warn_unused_result") == 0)) {
    attrs->nodiscard = true;
    attrs->nodiscard_msg = attribute_message(&tok, tok);
    return tok;
  }
  if (strcmp(name, "maybe_unused") == 0 || (vendor && strcmp(name, "unused") == 0)) {
    attrs->maybe_unused = true;
  } else if (strcmp(name, "noreturn") == 0 || strcmp(name, "_Noreturn") == 0) {
    attrs->noreturn = true;
  } else if (strcmp(name, "fallthrough") == 0) {
    attrs->fallthrough = true;
  } else if (vendor && strcmp(name, "packed") == 0) {
    attrs->packed = true;
  } else if (vendor && strcmp(name, "aligned") == 0) {
    if (tok_equal(tok, "(")) {
      attrs->aligned = (int)const_expr(&tok, tok->next);
      tok = tok_skip(tok, ")");
      if (!is_power_of_two((uint64_t)attrs->aligned))
        error_tok(name_tok, "requested alignment is not a power of 2");
    } else {
      attrs->aligned = 16;
    }
    return tok;
  } else if (!vendor && strcmp(name, "unsequenced") != 0 && strcmp(name, "reproducible") != 0) {
    warn_tok(W_UNSUPPORTED, name_tok, "unknown attribute '%s' ignored", name);
  }
  // Vendor attributes we do not model are accepted and ignored.
  if (tok_equal(tok, "("))
    tok = skip_balanced_parens(tok);
  return tok;
}

Token *parse_attributes(Token *tok, Attributes *attrs) {
  while (tok_equal(tok, "[") && tok_equal(tok->next, "[")) {
    tok = tok->next->next;
    while (!tok_equal(tok, "]")) {
      if (tok_consume(&tok, tok, ","))
        continue;
      if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD)
        error_tok(tok, "expected attribute name");
      Token *name = tok;
      bool vendor = false;
      tok = tok->next;
      if (tok_equal(tok, "::")) { // prefixed attribute, e.g. gnu::packed
        vendor = true;
        name = tok->next;
        tok = tok->next->next;
      }
      tok = apply_attribute(name, tok, attrs, vendor);
    }
    tok = tok_skip(tok_skip(tok, "]"), "]");
  }
  return skip_gnu_attributes(tok, attrs);
}

Token *skip_gnu_attributes(Token *tok, Attributes *attrs) {
  Attributes dummy = {};
  if (!attrs)
    attrs = &dummy;
  for (;;) {
    if (tok_equal(tok, "__attribute__")) {
      tok = tok_skip(tok_skip(tok->next, "("), "(");
      while (!tok_equal(tok, ")")) {
        if (tok_consume(&tok, tok, ","))
          continue;
        Token *name = tok;
        tok = apply_attribute(name, tok->next, attrs, true);
      }
      tok = tok_skip(tok_skip(tok, ")"), ")");
      continue;
    }
    // asm labels: int x __asm__("name");  (accepted, the symbol name is kept)
    if (tok_equal(tok, "__asm__") || tok_equal(tok, "__asm") || tok_equal(tok, "asm")) {
      tok = skip_balanced_parens(tok->next);
      continue;
    }
    return tok;
  }
}

// ---------------------------------------------------------------------------
// Declaration specifiers
// ---------------------------------------------------------------------------

bool is_typename(Token *tok) {
  static const char *const kw[] = {
      "void", "_Bool", "bool", "char", "short", "int", "long", "float", "double", "struct",
      "union", "typedef", "enum", "static", "extern", "_Alignas", "alignas", "signed", "unsigned",
      "const", "volatile", "auto", "register", "restrict", "__restrict", "__restrict__",
      "_Noreturn", "inline", "__inline", "__inline__", "_Thread_local", "thread_local",
      "__thread", "_Atomic", "typeof", "typeof_unqual", "__typeof", "__typeof__", "constexpr",
      "__attribute__", "__extension__", "__signed__", "__const", "__volatile__", "_Complex",
      "_Imaginary", "_BitInt", "_Decimal32", "_Decimal64", "_Decimal128",
  };
  if (tok->kind == TK_KEYWORD) {
    for (size_t i = 0; i < ARRAY_LEN(kw); i++)
      if (tok_equal(tok, kw[i]))
        return true;
    return false;
  }
  if (tok->kind != TK_IDENT)
    return false;
  VarScope *vs = find_var(tok);
  return vs && vs->type_def;
}

static Type *struct_union_decl(Token **rest, Token *tok, TypeKind kind);
static Type *enum_specifier(Token **rest, Token *tok);

static Type *typeof_specifier(Token **rest, Token *tok, bool unqual) {
  tok = tok_skip(tok, "(");
  Type *ty;
  if (is_typename(tok)) {
    ty = typename_(&tok, tok);
  } else {
    P.unevaluated++;
    Node *node = expr(&tok, tok);
    P.unevaluated--;
    ty = node->ty;
  }
  *rest = tok_skip(tok, ")");
  return unqual ? unqualified(ty) : ty;
}

static void storage_class_error(Token *tok, VarAttr *attr) {
  if (!attr)
    error_tok(tok, "storage class specifier '%.*s' is not allowed here", tok->len, tok->loc);
}

Type *declspec(Token **rest, Token *tok, VarAttr *attr) {
  // Each basic type keyword adds a distinct amount to `counter`, so every
  // valid combination ("unsigned long int", "long long", ...) maps to a
  // unique number.
  enum {
    VOID = 1 << 0,
    BOOL = 1 << 2,
    CHAR = 1 << 4,
    SHORT = 1 << 6,
    INT = 1 << 8,
    LONG = 1 << 10,
    FLOAT = 1 << 12,
    DOUBLE = 1 << 14,
    OTHER = 1 << 16,
    SIGNED = 1 << 17,
    UNSIGNED = 1 << 18,
  };

  Type *ty = ty_int;
  int counter = 0;
  bool is_const = false, is_volatile = false, saw_auto = false;
  Token *start = tok;
  static bool warned_long_double;

  for (;;) {
    if (tok_equal(tok, "[") && tok_equal(tok->next, "[")) {
      Attributes dummy = {};
      tok = parse_attributes(tok, attr ? &attr->attrs : &dummy);
      continue;
    }
    if (!is_typename(tok))
      break;

    // Storage classes and function specifiers.
    if (tok_equal(tok, "typedef") || tok_equal(tok, "static") || tok_equal(tok, "extern") ||
        tok_equal(tok, "inline") || tok_equal(tok, "__inline") || tok_equal(tok, "__inline__") ||
        tok_equal(tok, "_Thread_local") || tok_equal(tok, "thread_local") ||
        tok_equal(tok, "__thread") || tok_equal(tok, "constexpr") || tok_equal(tok, "register") ||
        tok_equal(tok, "auto") || tok_equal(tok, "_Noreturn")) {
      storage_class_error(tok, attr);
      if (tok_equal(tok, "typedef"))
        attr->is_typedef = true;
      else if (tok_equal(tok, "static"))
        attr->is_static = true;
      else if (tok_equal(tok, "extern"))
        attr->is_extern = true;
      else if (tok_equal(tok, "constexpr"))
        attr->is_constexpr = true;
      else if (tok_equal(tok, "register"))
        attr->is_register = true;
      else if (tok_equal(tok, "auto"))
        saw_auto = true;
      else if (tok_equal(tok, "_Noreturn")) {
        attr->is_noreturn = true;
        warn_tok(W_DEPRECATED, tok, "'_Noreturn' is obsolescent in C23; use [[noreturn]]");
      } else if (tok_equal(tok, "_Thread_local") || tok_equal(tok, "thread_local") ||
                 tok_equal(tok, "__thread"))
        attr->is_tls = true;
      else
        attr->is_inline = true;

      if (attr->is_typedef && (attr->is_static || attr->is_extern || attr->is_inline || attr->is_tls))
        error_tok(tok, "typedef may not be used together with other storage classes");
      tok = tok->next;
      continue;
    }

    // Qualifiers and ignored keywords.
    if (tok_equal(tok, "const") || tok_equal(tok, "__const")) {
      is_const = true;
      tok = tok->next;
      continue;
    }
    if (tok_equal(tok, "volatile") || tok_equal(tok, "__volatile__")) {
      is_volatile = true;
      tok = tok->next;
      continue;
    }
    if (tok_equal(tok, "restrict") || tok_equal(tok, "__restrict") ||
        tok_equal(tok, "__restrict__") || tok_equal(tok, "__extension__")) {
      tok = tok->next;
      continue;
    }
    if (tok_equal(tok, "__attribute__")) {
      tok = skip_gnu_attributes(tok, attr ? &attr->attrs : nullptr);
      continue;
    }
    if (tok_equal(tok, "_Atomic")) {
      warn_tok(W_UNSUPPORTED, tok, "_Atomic is accepted but operations are not atomic");
      tok = tok->next;
      if (tok_equal(tok, "(")) {
        if (counter)
          break;
        ty = typename_(&tok, tok->next);
        tok = tok_skip(tok, ")");
        counter += OTHER;
      }
      continue;
    }
    if (tok_equal(tok, "alignas") || tok_equal(tok, "_Alignas")) {
      if (!attr)
        error_tok(tok, "alignas is not allowed here");
      tok = tok_skip(tok->next, "(");
      int align = is_typename(tok) ? typename_(&tok, tok)->align : (int)const_expr(&tok, tok);
      if (align && !is_power_of_two((uint64_t)align))
        error_tok(tok, "requested alignment is not a positive power of 2");
      attr->align = MAX(attr->align, align);
      tok = tok_skip(tok, ")");
      continue;
    }
    if (tok_equal(tok, "_Complex") || tok_equal(tok, "_Imaginary") || tok_equal(tok, "_BitInt") ||
        starts_with(tok->loc, "_Decimal"))
      error_tok(tok, "'%.*s' is not supported by occ", tok->len, tok->loc);

    // User-defined and derived type specifiers.
    VarScope *td = tok->kind == TK_IDENT ? find_var(tok) : nullptr;
    if (td || tok_equal(tok, "struct") || tok_equal(tok, "union") || tok_equal(tok, "enum") ||
        tok_equal(tok, "typeof") || tok_equal(tok, "typeof_unqual") ||
        tok_equal(tok, "__typeof") || tok_equal(tok, "__typeof__")) {
      if (counter)
        break; // e.g. "int T" where T is a typedef name being redeclared
      if (tok_equal(tok, "struct")) {
        ty = struct_union_decl(&tok, tok->next, TY_STRUCT);
      } else if (tok_equal(tok, "union")) {
        ty = struct_union_decl(&tok, tok->next, TY_UNION);
      } else if (tok_equal(tok, "enum")) {
        ty = enum_specifier(&tok, tok->next);
      } else if (tok_equal(tok, "typeof_unqual")) {
        ty = typeof_specifier(&tok, tok->next, true);
      } else if (tok->kind == TK_KEYWORD) {
        ty = typeof_specifier(&tok, tok->next, false);
      } else {
        ty = td->type_def;
        tok = tok->next;
      }
      counter += OTHER;
      continue;
    }

    // Basic types.
    if (tok_equal(tok, "void"))
      counter += VOID;
    else if (tok_equal(tok, "_Bool") || tok_equal(tok, "bool"))
      counter += BOOL;
    else if (tok_equal(tok, "char"))
      counter += CHAR;
    else if (tok_equal(tok, "short"))
      counter += SHORT;
    else if (tok_equal(tok, "int"))
      counter += INT;
    else if (tok_equal(tok, "long"))
      counter += LONG;
    else if (tok_equal(tok, "float"))
      counter += FLOAT;
    else if (tok_equal(tok, "double"))
      counter += DOUBLE;
    else if (tok_equal(tok, "signed") || tok_equal(tok, "__signed__"))
      counter |= SIGNED;
    else if (tok_equal(tok, "unsigned"))
      counter |= UNSIGNED;
    else
      ICE_UNREACHABLE();

    switch (counter) {
    case VOID: ty = ty_void; break;
    case BOOL: ty = ty_bool; break;
    case CHAR: ty = ty_char; break;
    case SIGNED + CHAR: ty = ty_schar; break;
    case UNSIGNED + CHAR: ty = ty_uchar; break;
    case SHORT: case SHORT + INT: case SIGNED + SHORT: case SIGNED + SHORT + INT:
      ty = ty_short;
      break;
    case UNSIGNED + SHORT: case UNSIGNED + SHORT + INT: ty = ty_ushort; break;
    case INT: case SIGNED: case SIGNED + INT: ty = ty_int; break;
    case UNSIGNED: case UNSIGNED + INT: ty = ty_uint; break;
    case LONG: case LONG + INT: case SIGNED + LONG: case SIGNED + LONG + INT:
      ty = ty_long;
      break;
    case LONG + LONG: case LONG + LONG + INT: case SIGNED + LONG + LONG:
    case SIGNED + LONG + LONG + INT:
      ty = ty_llong;
      break;
    case UNSIGNED + LONG: case UNSIGNED + LONG + INT: ty = ty_ulong; break;
    case UNSIGNED + LONG + LONG: case UNSIGNED + LONG + LONG + INT: ty = ty_ullong; break;
    case FLOAT: ty = ty_float; break;
    case DOUBLE: ty = ty_double; break;
    case LONG + DOUBLE:
      ty = ty_ldouble;
      if (!warned_long_double && !(tok->file && tok->file->is_system)) {
        warned_long_double = true;
        warn_tok(W_UNSUPPORTED, tok, "'long double' is implemented as 'double' by occ");
      }
      break;
    default:
      error_tok(tok, "invalid combination of type specifiers");
    }
    tok = tok->next;
  }

  if (counter == 0) {
    if (!saw_auto || !attr)
      error_tok(start, "type specifier missing; ISO C99 and later do not support implicit int");
    attr->is_auto = true; // C23 type inference
  }

  *rest = tok;
  return qualified(ty, is_const, is_volatile);
}

// ---------------------------------------------------------------------------
// Declarators
// ---------------------------------------------------------------------------

static Type *pointers(Token **rest, Token *tok, Type *ty) {
  while (tok_consume(&tok, tok, "*")) {
    ty = pointer_to(ty);
    for (;;) {
      if (tok_equal(tok, "const") || tok_equal(tok, "__const")) {
        ty = qualified(ty, true, false);
      } else if (tok_equal(tok, "volatile") || tok_equal(tok, "__volatile__")) {
        ty = qualified(ty, false, true);
      } else if (tok_equal(tok, "restrict") || tok_equal(tok, "__restrict") ||
                 tok_equal(tok, "__restrict__") || tok_equal(tok, "_Atomic")) {
        // restrict is a promise to the optimizer; occ does not exploit it.
      } else if (tok_equal(tok, "__attribute__") || (tok_equal(tok, "[") && tok_equal(tok->next, "["))) {
        Attributes dummy = {};
        tok = parse_attributes(tok, &dummy);
        continue;
      } else {
        break;
      }
      tok = tok->next;
    }
  }
  *rest = tok;
  return ty;
}

static Type *type_suffix(Token **rest, Token *tok, Type *ty);

static Type *func_params(Token **rest, Token *tok, Type *ret) {
  if (ret->kind == TY_ARRAY)
    error_tok(tok, "function cannot return array type '%s'", type_name(ret));
  if (ret->kind == TY_FUNC)
    error_tok(tok, "function cannot return function type '%s'", type_name(ret));

  Type *fn = func_type(ret);
  if (tok_equal(tok, "void") && tok_equal(tok->next, ")")) {
    *rest = tok->next->next;
    return fn;
  }

  Type head = {};
  Type *cur = &head;
  while (!tok_equal(tok, ")")) {
    if (cur != &head)
      tok = tok_skip(tok, ",");
    if (tok_equal(tok, "...")) {
      fn->is_variadic = true;
      tok = tok->next;
      if (!tok_equal(tok, ")"))
        error_tok(tok, "'...' must be the last parameter");
      break;
    }

    Attributes attrs = {};
    tok = parse_attributes(tok, &attrs);
    if (!is_typename(tok)) {
      if (tok->kind == TK_IDENT)
        error_tok(tok, "unknown type name '%.*s' (K&R-style parameter lists were removed in C23)",
                  tok->len, tok->loc);
      error_tok(tok, "expected parameter declaration");
    }

    VarAttr pattr = {};
    Token *spec = tok;
    Type *base = declspec(&tok, tok, &pattr);
    if (pattr.is_typedef || pattr.is_static || pattr.is_extern || pattr.is_tls || pattr.is_inline)
      error_tok(spec, "invalid storage class for a parameter");
    DeclInfo di = {};
    Type *pty = declarator(&tok, tok, base, &di);

    // Parameter type adjustments (C23 6.7.7.4).
    bool was_array = false;
    if (pty->kind == TY_ARRAY) {
      pty = pointer_to(pty->base);
      was_array = true;
    } else if (pty->kind == TY_FUNC) {
      pty = pointer_to(pty);
    } else if (pty->kind == TY_VOID) {
      error_tok(di.name_pos, "parameter may not have 'void' type");
    }

    Type *p = copy_type(pty);
    p->name = di.name;
    p->name_pos = di.name_pos;
    p->param_was_array = was_array;
    cur = cur->next = p;
  }

  fn->params = head.next;
  *rest = tok->next;
  return fn;
}

static Type *array_dimensions(Token **rest, Token *tok, Type *ty) {
  Token *start = tok;
  while (tok_equal(tok, "static") || tok_equal(tok, "const") || tok_equal(tok, "volatile") ||
         tok_equal(tok, "restrict") || tok_equal(tok, "__restrict"))
    tok = tok->next;

  int64_t len = -1;
  if (tok_equal(tok, "*") && tok_equal(tok->next, "]"))
    error_tok(tok, "variable length arrays are not supported by occ");
  if (!tok_equal(tok, "]")) {
    Node *size = conditional(&tok, tok);
    if (!is_integer(size->ty))
      error_tok(start, "size of array has non-integer type '%s'", type_name(size->ty));
    if (!is_const_int_expr(size))
      error_tok(start, "variable length arrays are not supported by occ");
    len = eval_int(size);
    if (len < 0)
      error_tok(start, "size of array is negative");
    if (len > INT32_MAX)
      error_tok(start, "size of array is too large");
  }
  tok = tok_skip(tok, "]");

  ty = type_suffix(rest, tok, ty);
  if (ty->kind == TY_FUNC)
    error_tok(start, "declaration of an array of functions");
  if (!is_complete(ty))
    error_tok(start, "array has incomplete element type '%s'", type_name(ty));
  return array_of(ty, (int)len);
}

static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
  if (tok_equal(tok, "("))
    return func_params(rest, tok->next, ty);
  if (tok_equal(tok, "["))
    return array_dimensions(rest, tok->next, ty);
  *rest = tok;
  return ty;
}

// Decides whether '(' starts a nested declarator rather than a parameter list.
static bool is_nested_declarator(Token *tok) {
  if (tok_equal(tok, "*") || tok_equal(tok, "(") || tok_equal(tok, "[") || tok_equal(tok, "^"))
    return !tok_equal(tok, "[") || !tok_equal(tok->next, "[");
  return tok->kind == TK_IDENT && !is_typename(tok);
}

Type *declarator(Token **rest, Token *tok, Type *ty, DeclInfo *info) {
  ty = pointers(&tok, tok, ty);

  if (tok_equal(tok, "(") && is_nested_declarator(tok->next)) {
    // "T (inner) suffix": the suffix applies first, the inner declarator
    // wraps the result. Parse the inner part twice: once to skip it.
    Token *start = tok;
    Type placeholder = *ty_int;
    DeclInfo ignored = {};
    declarator(&tok, start->next, &placeholder, info ? &ignored : nullptr);
    tok = tok_skip(tok, ")");
    ty = type_suffix(rest, tok, ty);
    Token *unused;
    return declarator(&unused, start->next, ty, info);
  }

  Token *name_pos = tok;
  Token *name = nullptr;
  if (tok->kind == TK_IDENT) {
    if (!info)
      error_tok(tok, "unexpected identifier in type name");
    name = tok;
    tok = tok->next;
  }
  Attributes attrs = {};
  tok = parse_attributes(tok, &attrs);
  ty = type_suffix(&tok, tok, ty);
  tok = skip_gnu_attributes(tok, &attrs);
  if (info) {
    info->name = name;
    info->name_pos = name_pos;
    info->attrs = attrs;
  }
  *rest = tok;
  return ty;
}

Type *typename_(Token **rest, Token *tok) {
  Type *ty = declspec(&tok, tok, nullptr);
  return declarator(rest, tok, ty, nullptr);
}

// ---------------------------------------------------------------------------
// Struct and union
// ---------------------------------------------------------------------------

static Member *find_member_in(Member *list, const Token *name) {
  for (Member *m = list; m; m = m->next) {
    if (m->name && m->name->len == name->len && !memcmp(m->name->loc, name->loc, (size_t)name->len))
      return m;
    if (!m->name && (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION) &&
        find_member_in(m->ty->members, name))
      return m;
  }
  return nullptr;
}

static Member *struct_members(Token **rest, Token *tok, Type *owner) {
  Member head = {};
  Member *cur = &head;
  int idx = 0;

  while (!tok_equal(tok, "}")) {
    if (tok->kind == TK_EOF)
      error_tok(tok, "expected '}' at end of struct");
    if (tok_equal(tok, "static_assert") || tok_equal(tok, "_Static_assert")) {
      tok = static_assertion(tok);
      continue;
    }

    VarAttr attr = {};
    Token *spec = tok;
    Type *basety = declspec(&tok, tok, &attr);
    if (attr.is_typedef || attr.is_static || attr.is_extern || attr.is_tls || attr.is_register)
      error_tok(spec, "storage class is not allowed on a struct member");

    // Anonymous struct/union member (C11).
    if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION) && tok_equal(tok, ";")) {
      Member *mem = NEW(Member);
      mem->ty = basety;
      mem->tok = spec;
      mem->idx = idx++;
      mem->align = attr.align ? attr.align : basety->align;
      for (Member *m = basety->members; m; m = m->next)
        if (m->name && find_member_in(head.next, m->name))
          error_tok(m->name, "duplicate member '%.*s'", m->name->len, m->name->loc);
      cur = cur->next = mem;
      tok = tok->next;
      continue;
    }

    bool first = true;
    while (!tok_consume(&tok, tok, ";")) {
      if (!first)
        tok = tok_skip(tok, ",");
      first = false;

      Member *mem = NEW(Member);
      DeclInfo di = {.name_pos = tok};
      if (tok_equal(tok, ":"))
        mem->ty = basety; // unnamed bit-field
      else
        mem->ty = declarator(&tok, tok, basety, &di);
      mem->name = di.name;
      mem->tok = di.name ? di.name : di.name_pos;
      mem->idx = idx++;
      mem->align = attr.align ? attr.align : mem->ty->align;
      if (di.attrs.aligned)
        mem->align = MAX(mem->align, di.attrs.aligned);

      if (tok_consume(&tok, tok, ":")) {
        if (!is_integer(mem->ty))
          error_tok(mem->tok, "bit-field has non-integral type '%s'", type_name(mem->ty));
        Token *width_tok = tok;
        int64_t width = const_expr(&tok, tok);
        if (width < 0)
          error_tok(width_tok, "bit-field has negative width");
        if (width > mem->ty->size * 8)
          error_tok(width_tok, "width of bit-field (%lld bits) exceeds the width of its type (%d bits)",
                    (long long)width, mem->ty->size * 8);
        if (width == 0 && mem->name)
          error_tok(width_tok, "named bit-field has zero width");
        mem->is_bitfield = true;
        mem->bit_width = (int)width;
        tok = skip_gnu_attributes(tok, nullptr);
      }

      if (mem->name && find_member_in(head.next, mem->name))
        error_tok(mem->name, "duplicate member '%.*s'", mem->name->len, mem->name->loc);
      if (mem->ty->kind == TY_FUNC)
        error_tok(mem->tok, "member declared as a function");
      bool is_flexible = mem->ty->kind == TY_ARRAY && mem->ty->array_len < 0;
      if (!is_flexible && !is_complete(mem->ty))
        error_tok(mem->tok, "member has incomplete type '%s'", type_name(mem->ty));
      cur = cur->next = mem;
    }
  }

  // Only the last member may be a flexible array.
  for (Member *m = head.next; m; m = m->next) {
    if (m->ty->kind == TY_ARRAY && m->ty->array_len < 0) {
      if (m->next || owner->kind == TY_UNION)
        error_tok(m->tok, "flexible array member must be the last member of a struct");
      if (m == head.next)
        error_tok(m->tok, "flexible array member in a struct with no named members");
      owner->is_flexible = true;
    }
  }

  *rest = tok->next;
  return head.next;
}

static int member_size(const Member *m) {
  return m->ty->kind == TY_ARRAY && m->ty->array_len < 0 ? 0 : m->ty->size;
}

// A member's alignment inside a struct, after #pragma pack.
static int member_align(const Type *ty, const Member *m) {
  return ty->max_align ? MIN(m->align, ty->max_align) : m->align;
}

static void layout_struct(Type *ty) {
  int64_t bits = 0;
  int align = 1;
  for (Member *m = ty->members; m; m = m->next) {
    if (m->is_bitfield && ty->max_align && ty->max_align < m->align) {
      // Under #pragma pack a bit-field may straddle its storage unit, and
      // the unit is only aligned to the pack value.
      if (m->bit_width == 0)
        continue;
      m->offset = (int)(bits / 8 / ty->max_align * ty->max_align);
      m->bit_offset = (int)(bits - (int64_t)m->offset * 8);
      if (m->bit_offset + m->bit_width > m->ty->size * 8) { // must fit one load
        bits = align_to(bits, 8);
        m->offset = (int)(bits / 8);
        m->bit_offset = 0;
      }
      bits += m->bit_width;
      if (m->name)
        align = MAX(align, member_align(ty, m));
      continue;
    }
    if (m->is_bitfield) {
      int unit = m->ty->size * 8;
      if (m->bit_width == 0) {
        bits = align_to(bits, unit);
        continue;
      }
      // A bit-field never straddles a storage unit of its declared type.
      if (!ty->is_packed && bits / unit != (bits + m->bit_width - 1) / unit)
        bits = align_to(bits, unit);
      m->offset = (int)(bits / unit * m->ty->size);
      m->bit_offset = (int)(bits - (int64_t)m->offset * 8);
      bits += m->bit_width;
      if (m->name && !ty->is_packed)
        align = MAX(align, m->align);
      continue;
    }
    if (!ty->is_packed)
      bits = align_to(bits, (int64_t)member_align(ty, m) * 8);
    m->offset = (int)(bits / 8);
    bits += (int64_t)member_size(m) * 8;
    if (!ty->is_packed)
      align = MAX(align, member_align(ty, m));
  }
  ty->align = align;
  ty->size = (int)(align_to(bits, (int64_t)align * 8) / 8);
}

static void layout_union(Type *ty) {
  int size = 0, align = 1;
  for (Member *m = ty->members; m; m = m->next) {
    m->offset = 0;
    size = MAX(size, member_size(m));
    if (!ty->is_packed && (m->name || !m->is_bitfield))
      align = MAX(align, member_align(ty, m));
  }
  ty->align = align;
  ty->size = (int)align_to(size, align);
}

static Type *struct_union_decl(Token **rest, Token *tok, TypeKind kind) {
  Attributes attrs = {};
  tok = parse_attributes(tok, &attrs);
  const char *what = kind == TY_STRUCT ? "struct" : "union";

  Token *tag = nullptr;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  if (tag && !tok_equal(tok, "{")) {
    Type *ty = find_tag(tag);
    // "struct S;" alone declares a new type in the current scope.
    bool is_forward_decl = tok_equal(tok, ";");
    if (ty && !(is_forward_decl && !find_tag_in_current_scope(tag))) {
      if (ty->kind != kind)
        error_tok(tag, "use of '%.*s' with tag type that does not match previous declaration",
                  tag->len, tag->loc);
      *rest = tok;
      return ty;
    }
    ty = struct_type(kind);
    ty->tag = tag;
    push_tag(tag, ty);
    *rest = tok;
    return ty;
  }
  if (!tag && !tok_equal(tok, "{"))
    error_tok(tok, "expected '{' or a tag name after '%s'", what);
  int pack = tok->pack; // #pragma pack in effect at the definition
  tok = tok->next;

  Type *ty = nullptr;
  if (tag) {
    Type *existing = find_tag_in_current_scope(tag);
    if (existing) {
      if (existing->kind != kind)
        error_tok(tag, "use of '%.*s' with tag type that does not match previous declaration",
                  tag->len, tag->loc);
      if (existing->size >= 0)
        error_tok(tag, "redefinition of '%s %.*s'", what, tag->len, tag->loc);
      ty = existing;
    }
  }
  if (!ty) {
    ty = struct_type(kind);
    ty->tag = tag;
    if (tag)
      push_tag(tag, ty);
  }

  ty->members = struct_members(&tok, tok, ty);
  tok = parse_attributes(tok, &attrs);
  ty->is_packed = attrs.packed;
  ty->max_align = pack;
  if (kind == TY_STRUCT)
    layout_struct(ty);
  else
    layout_union(ty);
  if (attrs.aligned)
    ty->align = MAX(ty->align, attrs.aligned), ty->size = (int)align_to(ty->size, ty->align);
  complete_struct(ty);

  *rest = tok;
  return ty;
}

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

static void set_enum_base(Type *ty, Type *base) {
  ty->base = base;
  ty->size = base->size;
  ty->align = base->align;
  ty->is_unsigned = base->is_unsigned;
}

static Type *enum_specifier(Token **rest, Token *tok) {
  Attributes attrs = {};
  tok = parse_attributes(tok, &attrs);

  Token *tag = nullptr;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  Type *fixed = nullptr;
  if (tok_equal(tok, ":")) {
    Token *start = tok->next;
    fixed = unqualified(declspec(&tok, tok->next, nullptr));
    if (!is_integer(fixed) || fixed->kind == TY_ENUM)
      error_tok(start, "enum underlying type must be an integer type, not '%s'", type_name(fixed));
  }

  if (tag && !tok_equal(tok, "{")) {
    Type *ty = find_tag(tag);
    if (ty) {
      if (ty->kind != TY_ENUM)
        error_tok(tag, "use of '%.*s' with tag type that does not match previous declaration",
                  tag->len, tag->loc);
      *rest = tok;
      return ty;
    }
    ty = enum_type();
    ty->tag = tag;
    if (fixed) {
      set_enum_base(ty, fixed);
      ty->is_fixed_enum = true;
    }
    push_tag(tag, ty);
    *rest = tok;
    return ty;
  }

  tok = tok_skip(tok, "{");
  Type *ty = enum_type();
  ty->tag = tag;
  if (tag) {
    Type *existing = find_tag_in_current_scope(tag);
    if (existing && existing->kind == TY_ENUM && existing->is_fixed_enum && fixed)
      ty = existing; // completing "enum E : T;"
    else if (existing && existing->is_defined)
      error_tok(tag, "redefinition of 'enum %.*s'", tag->len, tag->loc);
    push_tag(tag, ty);
  }
  if (fixed) {
    set_enum_base(ty, fixed);
    ty->is_fixed_enum = true;
  }

  typedef VEC(VarScope *) ScopeVec;
  ScopeVec consts = {};
  int64_t val = 0, min = 0, max = 0;
  bool first = true, any_unsigned_big = false;
  while (!tok_equal(tok, "}")) {
    if (!first)
      tok = tok_skip(tok, ",");
    if (tok_equal(tok, "}"))
      break;
    if (tok->kind != TK_IDENT)
      error_tok(tok, "expected identifier in enumerator list");
    Token *name = tok;
    Attributes eattrs = {};
    tok = parse_attributes(tok->next, &eattrs);
    if (tok_equal(tok, "=")) {
      Token *vtok = tok->next;
      Node *v = conditional(&tok, tok->next);
      if (!is_integer(v->ty) || !is_const_int_expr(v))
        error_tok(vtok, "enumerator value is not an integer constant expression");
      val = eval_int(v);
      if (v->ty->is_unsigned && v->ty->size == 8 && val < 0)
        any_unsigned_big = true;
    } else if (!first && val == INT64_MIN) {
      error_tok(name, "enumerator value overflows");
    }
    if (fixed) {
      int bits = fixed->size * 8;
      bool ok = fixed->kind == TY_BOOL ? (val == 0 || val == 1)
                : fixed->is_unsigned  ? (bits == 64 || ((uint64_t)val >> bits) == 0)
                                      : (bits == 64 || (val >= -(INT64_C(1) << (bits - 1)) &&
                                                        val < (INT64_C(1) << (bits - 1))));
      if (!ok)
        error_tok(name, "enumerator value %lld is not representable in the underlying type '%s'",
                  (long long)val, type_name(fixed));
    }
    if (find_var_in_current_scope(name))
      error_tok(name, "redefinition of '%.*s'", name->len, name->loc);
    VarScope *vs = push_scope(tok_text(name));
    vs->enum_ty = ty;
    vs->enum_val = val;
    vs->tok = name;
    vec_push(&consts, vs);
    min = first ? val : MIN(min, val);
    max = first ? val : MAX(max, val);
    first = false;
    val++;
  }
  *rest = tok->next;
  ty->is_defined = true;

  if (!fixed) {
    // C23 6.7.3.3: choose an underlying type able to represent all values.
    Type *base;
    if (any_unsigned_big)
      base = ty_ulong;
    else if (min >= INT32_MIN && max <= INT32_MAX)
      base = ty_int;
    else if (min >= 0 && max <= UINT32_MAX)
      base = ty_uint;
    else
      base = ty_long;
    set_enum_base(ty, base);
  }
  return ty;
}

// ---------------------------------------------------------------------------
// static_assert
// ---------------------------------------------------------------------------

Token *static_assertion(Token *tok) {
  Token *start = tok;
  tok = tok_skip(tok->next, "(");
  Token *cond_tok = tok;
  Node *cond = conditional(&tok, tok);
  if (!is_integer(cond->ty) || !is_const_int_expr(cond))
    error_tok(cond_tok, "static assertion expression is not an integral constant expression");
  char *msg = nullptr;
  if (tok_consume(&tok, tok, ",")) {
    if (tok->kind != TK_STR)
      error_tok(tok, "expected string literal in static_assert");
    msg = tok->str;
    tok = tok->next;
  }
  tok = tok_skip(tok_skip(tok, ")"), ";");
  if (eval_int(cond) == 0) {
    if (msg)
      error_tok(start, "static assertion failed: %s", msg);
    error_tok(start, "static assertion failed");
  }
  return tok;
}

// ---------------------------------------------------------------------------
// Typedefs, variables and functions
// ---------------------------------------------------------------------------

static void declare_typedef(Token *name, Type *ty) {
  VarScope *old = find_var_in_current_scope(name);
  if (old) {
    if (old->type_def && types_compatible(old->type_def, ty))
      return; // C11: redefinition with the same type is allowed
    error_tok_nofatal(name, "redefinition of '%.*s'%s", name->len, name->loc,
                      old->type_def ? " with a different type" : " as different kind of symbol");
    if (old->tok)
      note_tok(old->tok, "previous definition is here");
    diag_abort();
  }
  VarScope *vs = push_scope(tok_text(name));
  vs->type_def = ty;
  vs->tok = name;
}

static void apply_decl_attrs(Obj *var, const Attributes *a) {
  if (a->deprecated) {
    var->is_deprecated = true;
    var->deprecated_msg = a->deprecated_msg;
  }
  if (a->nodiscard) {
    var->is_nodiscard = true;
    var->nodiscard_msg = a->nodiscard_msg;
  }
  if (a->noreturn)
    var->is_noreturn = true;
  if (a->maybe_unused)
    var->maybe_unused = true;
  if (a->aligned)
    var->align = MAX(var->align, a->aligned);
}

static Attributes merge_attrs(Attributes a, const Attributes *b) {
  a.deprecated |= b->deprecated;
  if (b->deprecated_msg)
    a.deprecated_msg = b->deprecated_msg;
  a.nodiscard |= b->nodiscard;
  if (b->nodiscard_msg)
    a.nodiscard_msg = b->nodiscard_msg;
  a.maybe_unused |= b->maybe_unused;
  a.noreturn |= b->noreturn;
  a.packed |= b->packed;
  a.aligned = MAX(a.aligned, b->aligned);
  return a;
}

static Obj *find_global(const Token *name) {
  for (Obj *g = P.globals; g; g = g->next)
    if (strlen(g->name) == (size_t)name->len && !memcmp(g->name, name->loc, (size_t)name->len))
      return g;
  return nullptr;
}

// Finds or creates the file-scope object for a declaration. Block-scope
// extern and function declarations refer to the same file-scope entity.
static Obj *declare_global(Token *name, Type *ty, VarAttr *attr, bool is_function) {
  Obj *var;
  if (at_file_scope()) {
    VarScope *vs = find_var_in_current_scope(name);
    if (vs && !vs->var) {
      error_tok_nofatal(name, "redefinition of '%.*s' as different kind of symbol", name->len, name->loc);
      if (vs->tok)
        note_tok(vs->tok, "previous definition is here");
      diag_abort();
    }
    var = vs ? vs->var : nullptr;
  } else {
    var = find_global(name);
  }

  if (var) {
    if (var->is_function != is_function || !types_compatible(var->ty, ty)) {
      error_tok_nofatal(name, "conflicting types for '%s'", var->name);
      if (var->tok)
        note_tok(var->tok, "previous declaration is here (type '%s')", type_name(var->ty));
      diag_abort();
    }
    if (ty->kind == TY_ARRAY && ty->array_len >= 0)
      var->ty = ty; // completes "extern int a[];"
    if (is_function && (!attr->is_inline || attr->is_extern))
      var->is_inline_def = false;
    if (attr->is_static && !var->is_static)
      error_tok(name, "static declaration of '%s' follows non-static declaration", var->name);
    if (!at_file_scope()) {
      VarScope *alias = push_scope(var->name);
      alias->var = var;
      alias->tok = name;
    }
  } else {
    var = new_gvar(tok_text(name), ty); // also enters the name into the current scope
    var->tok = name;
    var->is_function = is_function;
    var->is_definition = false;
    var->is_static = attr->is_static;
    var->is_inline_def = is_function && attr->is_inline && !attr->is_extern;
  }

  var->is_noreturn |= attr->is_noreturn;
  var->is_inline |= attr->is_inline;
  apply_decl_attrs(var, &attr->attrs);
  return var;
}

static void check_main(Token *name, Type *ty) {
  if (name->len != 4 || memcmp(name->loc, "main", 4) != 0)
    return;
  if (ty->return_ty->kind != TY_INT || ty->return_ty->is_unsigned)
    warn_tok(W_MAIN, name, "return type of 'main' is not 'int'");
  int n = 0;
  for (Type *p = ty->params; p; p = p->next)
    n++;
  if (n != 0 && n != 2 && n != 3)
    warn_tok(W_MAIN, name, "'main' takes only zero, two or three arguments");
}

static void resolve_goto_labels(void) {
  for (Node *g = P.gotos; g; g = g->goto_next) {
    Node *target = nullptr;
    for (Node *l = P.labels; l; l = l->goto_next)
      if (strcmp(g->label_name, l->label_name) == 0)
        target = l;
    if (!target)
      error_tok(g->tok->next, "use of undeclared label '%s'", g->label_name);
    g->label_id = target->label_id;
  }
  P.gotos = P.labels = nullptr;
}

static Token *function_definition(Token *tok, Type *ty, DeclInfo *di, VarAttr *attr) {
  Obj *fn = declare_global(di->name, ty, attr, true);
  if (fn->body)
    error_tok(di->name, "redefinition of '%s'", fn->name);
  check_main(di->name, ty);

  fn->is_definition = true;
  fn->ty = ty; // the definition's parameter names are the ones that matter
  fn->tok = di->name;
  fn->locals = nullptr;
  P.current_fn = fn;
  P.gotos = P.labels = nullptr;

  enter_scope();
  Obj head = {};
  Obj *cur = &head;
  for (Type *p = ty->params; p; p = p->next) {
    if (!is_complete(p) && p->kind != TY_PTR)
      error_tok(p->name_pos, "parameter has incomplete type '%s'", type_name(p));
    char *name = p->name ? tok_text(p->name) : "";
    if (p->name && find_var_in_current_scope(p->name))
      error_tok(p->name, "redefinition of parameter '%s'", name);
    Obj *var = new_lvar(name, p, p->name);
    var->is_param = true;
    cur = cur->next_param = var;
  }
  fn->params = head.next_param;

  if (ty->is_variadic)
    fn->va_area = new_temp(array_of(ty_char, 176));

  Token *body_start = tok;
  fn->body = compound_stmt(&tok, tok);
  leave_scope();
  resolve_goto_labels();

  bool is_main = strcmp(fn->name, "main") == 0;
  if (ty->return_ty->kind != TY_VOID && !is_main && stmt_falls_through(fn->body)) {
    Token *end = body_start;
    for (Token *t = body_start; t != tok; t = t->next)
      end = t;
    warn_tok(W_RETURN_TYPE, end, "non-void function '%s' does not return a value in all control paths",
             fn->name);
  }
  (void)attr;
  P.current_fn = nullptr;
  return tok;
}

static void global_variable(Token **rest, Token *tok, Type *ty, DeclInfo *di, VarAttr *attr) {
  if (ty->kind == TY_VOID)
    error_tok(di->name, "variable '%.*s' has incomplete type 'void'", di->name->len, di->name->loc);

  Obj *var = declare_global(di->name, ty, attr, false);
  var->is_tls |= attr->is_tls;
  if (attr->align)
    var->align = MAX(var->align, attr->align);

  var->is_constexpr = attr->is_constexpr;
  if (tok_equal(tok, "=")) {
    if (var->init_data)
      error_tok(di->name, "redefinition of '%s'", var->name);
    gvar_initializer(&tok, tok->next, var);
    var->is_definition = true;
    var->is_tentative = false;
  } else if (!attr->is_extern) {
    if (attr->is_constexpr)
      error_tok(di->name, "constexpr variable '%s' must be initialized", var->name);
    if (!var->init_data) {
      var->is_definition = true;
      var->is_tentative = true;
    }
  }
  if (attr->is_constexpr) {
    var->is_constexpr = true;
    var->ty = qualified(var->ty, true, false);
  }
  if (var->is_definition && !is_complete(var->ty) && !(var->ty->kind == TY_ARRAY && var->is_tentative))
    error_tok(di->name, "variable '%s' has incomplete type '%s'", var->name, type_name(var->ty));
  *rest = tok;
}

Token *global_declaration(Token *tok) {
  if (tok_equal(tok, "static_assert") || tok_equal(tok, "_Static_assert"))
    return static_assertion(tok);
  if (tok_equal(tok, ";"))
    return tok->next;
  if (tok_equal(tok, "asm") || tok_equal(tok, "__asm__") || tok_equal(tok, "__asm"))
    error_tok(tok, "top-level asm is not supported by occ");

  VarAttr attr = {};
  Token *start = tok;
  tok = parse_attributes(tok, &attr.attrs);
  if (!is_typename(tok)) {
    if (tok->kind == TK_IDENT && (tok_equal(tok->next, "(") || tok_equal(tok->next, ";") ||
                                  tok_equal(tok->next, "=") || tok_equal(tok->next, ",")))
      error_tok(tok, "type specifier missing; ISO C99 and later do not support implicit int");
    if (tok->kind == TK_IDENT)
      error_tok(tok, "unknown type name '%.*s'", tok->len, tok->loc);
    error_tok(tok, "expected a declaration");
  }
  Type *basety = declspec(&tok, tok, &attr);
  if (attr.is_auto)
    error_tok(start, "file-scope 'auto' declarations are not supported");
  if (tok_consume(&tok, tok, ";"))
    return tok; // just a struct/union/enum declaration

  for (bool first = true;; first = false) {
    DeclInfo di = {};
    Type *ty = declarator(&tok, tok, basety, &di);
    if (!di.name)
      error_tok(di.name_pos, "expected identifier or '('");
    VarAttr dattr = attr;
    dattr.attrs = merge_attrs(attr.attrs, &di.attrs);

    if (attr.is_typedef) {
      declare_typedef(di.name, ty);
    } else if (ty->kind == TY_FUNC) {
      if (first && tok_equal(tok, "{"))
        return function_definition(tok->next, ty, &di, &dattr);
      if (attr.is_tls || attr.is_constexpr)
        error_tok(di.name, "invalid storage class for a function");
      declare_global(di.name, ty, &dattr, true);
    } else {
      global_variable(&tok, tok, ty, &di, &dattr);
    }

    if (tok_consume(&tok, tok, ";"))
      return tok;
    tok = tok_skip(tok, ",");
  }
}

// ---------------------------------------------------------------------------
// Block-scope declarations
// ---------------------------------------------------------------------------

static Node *static_local(Token **rest, Token *tok, Type *ty, DeclInfo *di, VarAttr *attr) {
  Obj *var = new_anon_gvar(ty);
  var->name = format("%.*s.%s", di->name->len, di->name->loc, unique_name("static") + 3);
  var->tok = di->name;
  var->is_tls = attr->is_tls;
  apply_decl_attrs(var, &attr->attrs);
  if (attr->align)
    var->align = MAX(var->align, attr->align);
  VarScope *vs = push_scope(tok_text(di->name));
  vs->var = var;
  vs->tok = di->name;
  var->is_constexpr = attr->is_constexpr;
  if (tok_equal(tok, "="))
    gvar_initializer(&tok, tok->next, var);
  else if (attr->is_constexpr)
    error_tok(di->name, "constexpr variable must be initialized");
  if (attr->is_constexpr) {
    var->is_constexpr = true;
    var->ty = qualified(var->ty, true, false);
  }
  if (!is_complete(var->ty))
    error_tok(di->name, "variable has incomplete type '%s'", type_name(var->ty));
  *rest = tok;
  return nullptr;
}

Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  Node head = {};
  Node *cur = &head;
  Token *start = tok;

  for (int i = 0; !tok_equal(tok, ";"); i++) {
    if (i > 0)
      tok = tok_skip(tok, ",");

    DeclInfo di = {};
    Type *ty = declarator(&tok, tok, basety, &di);
    if (!di.name)
      error_tok(di.name_pos, "expected identifier");
    VarAttr dattr = *attr;
    dattr.attrs = merge_attrs(attr->attrs, &di.attrs);

    if (attr->is_typedef) {
      declare_typedef(di.name, ty);
      continue;
    }
    if (ty->kind == TY_FUNC) {
      if (tok_equal(tok, "{"))
        error_tok(tok, "function definitions are not allowed inside functions");
      declare_global(di.name, ty, &dattr, true);
      continue;
    }
    if (attr->is_extern) {
      if (tok_equal(tok, "="))
        error_tok(tok, "'extern' variable cannot have an initializer here");
      declare_global(di.name, ty, &dattr, false);
      continue;
    }
    VarScope *prev = find_var_in_current_scope(di.name);
    if (prev) {
      error_tok_nofatal(di.name, "redefinition of '%.*s'", di.name->len, di.name->loc);
      if (prev->tok)
        note_tok(prev->tok, "previous definition is here");
      diag_abort();
    }
    if (attr->is_static || attr->is_tls) {
      static_local(&tok, tok, ty, &di, &dattr);
      continue;
    }

    if (attr->is_auto) {
      // C23 auto: the type is that of the initializer after lvalue conversion.
      if (!tok_equal(tok, "="))
        error_tok(di.name, "declaration of '%.*s' with deduced type 'auto' requires an initializer",
                  di.name->len, di.name->loc);
      Token *init_tok = tok->next;
      Node *init = rvalue(assign(&tok, init_tok));
      if (init->ty->kind == TY_VOID)
        error_tok(init_tok, "variable has incomplete type 'void'");
      Obj *var = new_lvar(tok_text(di.name), unqualified(init->ty), di.name);
      apply_decl_attrs(var, &dattr.attrs);
      var->has_init = true;
      var->writes++;
      Node *lhs = new_var_node(var, di.name);
      Node *asg = new_binary(ND_ASSIGN, lhs, init, init_tok);
      asg->ty = var->ty;
      cur = cur->next = new_unary(ND_EXPR_STMT, asg, init_tok);
      continue;
    }

    if (ty->kind == TY_VOID)
      error_tok(di.name, "variable has incomplete type 'void'");

    Obj *var = new_lvar(tok_text(di.name), ty, di.name);
    apply_decl_attrs(var, &dattr.attrs);
    var->is_register = attr->is_register;
    var->is_constexpr = attr->is_constexpr;
    if (attr->align)
      var->align = MAX(var->align, attr->align);

    if (tok_equal(tok, "=")) {
      Token *eq = tok;
      Node *init = lvar_initializer(&tok, tok->next, var);
      cur = cur->next = new_unary(ND_EXPR_STMT, init, eq);
      var->has_init = true;
      var->writes++;
    } else if (attr->is_constexpr) {
      error_tok(di.name, "constexpr variable must be initialized");
    }

    if (attr->is_constexpr) {
      var->is_constexpr = true;
      var->ty = qualified(var->ty, true, false);
    }
    if (!is_complete(var->ty))
      error_tok(di.name, "variable has incomplete type '%s'", type_name(var->ty));
  }

  Node *node = new_node(ND_BLOCK, start);
  node->body = head.next;
  *rest = tok->next;
  return node;
}

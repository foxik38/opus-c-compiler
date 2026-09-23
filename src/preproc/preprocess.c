// preprocess.c - directives, file inclusion and conditional compilation.
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "preproc/pp_internal.h"
#include "support/hashmap.h"
#include "support/strbuf.h"

PreprocStats pp_stats;

typedef struct {
  char *path;
  bool is_system;
} IncludeDir;

typedef VEC(IncludeDir) IncludeDirVec;

typedef enum { IN_THEN, IN_ELIF, IN_ELSE } CondCtx;

typedef struct CondIncl CondIncl;
struct CondIncl {
  CondIncl *next;
  CondCtx ctx;
  Token *tok;
  bool included;
};

static struct {
  IncludeDirVec dirs;
  int first_angle_dir; // #include <...> starts searching here (skips -iquote dirs)
  CondIncl *cond;
  HashMap pragma_once;    // realpath -> 1
  HashMap include_guards; // path -> guard macro name
  HashMap file_cache;     // path -> SourceFile* (avoids re-reading headers)
  char *base_file;
  int pack;           // current #pragma pack value (0 = natural alignment)
  int pack_stack[32]; // #pragma pack(push)
  int pack_depth;
} pp;

enum { MAX_INCLUDE_DEPTH = 200 };

static Token *preprocess2(Token *tok);

// ---------------------------------------------------------------------------
// Include search
// ---------------------------------------------------------------------------

static bool file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *dirname_of(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash)
    return xstrdup(".");
  return xstrndup(path, (size_t)(slash - path));
}

// Resolves an include name. *found_index receives the matching search-list index.
static char *search_include(const char *name, bool is_quote, const Token *from, int start,
                            int *found_index) {
  *found_index = -1;
  if (name[0] == '/')
    return file_exists(name) ? xstrdup(name) : nullptr;

  if (is_quote && start < 0 && from && from->file) {
    char *path = format("%s/%s", dirname_of(from->file->path), name);
    if (file_exists(path))
      return path;
  }

  int begin = start >= 0 ? start : is_quote ? 0 : pp.first_angle_dir;
  for (int i = begin; i < (int)pp.dirs.len; i++) {
    char *path = format("%s/%s", pp.dirs.data[i].path, name);
    if (file_exists(path)) {
      *found_index = i;
      return path;
    }
  }
  return nullptr;
}

typedef enum {
  HDR_LINE,    // #include: a macro-expanded name spans the rest of the line
  HDR_PAREN,   // __has_include(...): a macro-expanded name ends at ')'
  HDR_LITERAL, // #embed: only literal names
} HeaderNameMode;

// Reads the header name of #include / #embed / __has_include. The name is
// taken either literally ("..." or <...>) or from macro-expanded tokens.
static char *read_header_name(Token **rest, Token *tok, bool *is_quote, HeaderNameMode mode) {
  if (tok->kind == TK_STR) {
    // The spelling is used verbatim: escapes are not processed in header names.
    *is_quote = true;
    *rest = tok->next;
    return xstrndup(tok->loc + 1, (size_t)tok->len - 2);
  }
  if (tok_equal(tok, "<")) {
    Token *start = tok;
    StrBuf sb = {};
    for (tok = tok->next; !tok_equal(tok, ">"); tok = tok->next) {
      if (tok->at_bol || tok->kind == TK_EOF)
        error_tok(start, "expected '>'");
      if (tok->has_space && sb.len)
        sb_putc(&sb, ' ');
      sb_append(&sb, tok->loc, (size_t)tok->len);
    }
    *is_quote = false;
    *rest = tok->next;
    return sb.data ? sb.data : xstrdup("");
  }
  if (tok->kind == TK_IDENT && mode != HDR_LITERAL) {
    Token *line = mode == HDR_LINE ? pp_copy_line(rest, tok) : nullptr;
    if (mode == HDR_PAREN) {
      // Inside __has_include(...): expand up to the closing parenthesis.
      Token head = {};
      Token *cur = &head;
      int depth = 0;
      for (; !(depth == 0 && tok_equal(tok, ")")); tok = tok->next) {
        if (tok->kind == TK_EOF)
          error_tok(tok, "expected ')'");
        depth += tok_equal(tok, "(") - tok_equal(tok, ")");
        cur = cur->next = tok_copy(tok);
      }
      cur->next = tok_new_eof(tok);
      line = head.next;
      *rest = tok;
    }
    Token *after;
    return read_header_name(&after, pp_expand(line), is_quote, HDR_LITERAL);
  }
  error_tok(tok, "expected \"FILENAME\" or <FILENAME>");
}

// Skips to the end of the directive line, warning about leftover tokens.
static Token *skip_line(Token *tok) {
  if (tok->at_bol)
    return tok;
  warn_tok(W_CPP, tok, "extra tokens at end of directive");
  while (!tok->at_bol)
    tok = tok->next;
  return tok;
}

static SourceFile *open_source(const char *path) {
  SourceFile *f = hashmap_get(&pp.file_cache, path);
  if (f)
    return f;
  f = load_source(path);
  if (f) {
    hashmap_put(&pp.file_cache, path, f);
    pp_stats.files++;
  }
  return f;
}

// Detects the "#ifndef X / #define X ... #endif" include-guard idiom.
static char *detect_include_guard(Token *tok) {
  if (!tok->at_bol || !tok_equal(tok, "#") || !tok_equal(tok->next, "ifndef"))
    return nullptr;
  Token *name = tok->next->next;
  if (name->kind != TK_IDENT || !tok_equal(name->next, "#") || !tok_equal(name->next->next, "define") ||
      name->next->next->next->len != name->len ||
      memcmp(name->next->next->next->loc, name->loc, (size_t)name->len) != 0)
    return nullptr;

  int depth = 0;
  for (tok = name->next; tok->kind != TK_EOF; tok = tok->next) {
    if (!tok->at_bol || !tok_equal(tok, "#"))
      continue;
    Token *d = tok->next;
    if (tok_equal(d, "if") || tok_equal(d, "ifdef") || tok_equal(d, "ifndef")) {
      depth++;
    } else if (tok_equal(d, "endif")) {
      if (depth-- == 0) {
        // The guard's #endif must be the last thing in the file.
        Token *t = d->next;
        while (!t->at_bol)
          t = t->next;
        return t->kind == TK_EOF ? tok_text(name) : nullptr;
      }
    } else if (depth == 0 && (tok_equal(d, "else") || tok_equal(d, "elif") ||
                              tok_equal(d, "elifdef") || tok_equal(d, "elifndef"))) {
      return nullptr;
    }
  }
  return nullptr;
}

static char *canonical_path(const char *path) {
  char buf[PATH_MAX];
  return realpath(path, buf) ? xstrdup(buf) : xstrdup(path);
}

static Token *include_file(Token *rest, const char *path, int dir_index, const Token *from) {
  char *canon = canonical_path(path);
  if (hashmap_get(&pp.pragma_once, canon))
    return rest;

  char *guard = hashmap_get(&pp.include_guards, canon);
  if (guard) {
    Token probe = {.kind = TK_IDENT, .loc = guard, .len = (int)strlen(guard)};
    if (macro_is_defined(&probe))
      return rest;
  }

  SourceFile *file = open_source(path);
  if (!file) {
    if (from)
      error_tok(from, "cannot open '%s'", path);
    fatal("cannot open '%s': no such file", path);
  }
  file->include_index = dir_index;
  if (dir_index >= 0 && pp.dirs.data[dir_index].is_system)
    file->is_system = true;

  file->depth = from && from->file ? from->file->depth + 1 : 0;
  if (file->depth > MAX_INCLUDE_DEPTH)
    error_tok(from, "#include nested too deeply (recursive include?)");

  Token *tok = tokenize(file);
  guard = detect_include_guard(tok);
  if (guard)
    hashmap_put(&pp.include_guards, canon, guard);
  return pp_append(tok, rest);
}

bool pp_has_include(Token *tok, Token **rest) {
  // tok is right after "__has_include"
  tok = tok_skip(tok, "(");
  bool is_quote;
  char *name = read_header_name(&tok, tok, &is_quote, HDR_PAREN);
  *rest = tok_skip(tok, ")");
  int idx;
  return search_include(name, is_quote, tok, -1, &idx) != nullptr;
}

// ---------------------------------------------------------------------------
// #embed (C23)
// ---------------------------------------------------------------------------

typedef struct {
  long limit; // -1: unlimited
  Token *prefix, *suffix, *if_empty;
} EmbedParams;

static Token *read_paren_group(Token **rest, Token *tok) {
  Token *start = tok;
  tok = tok_skip(tok, "(");
  Token head = {};
  Token *cur = &head;
  for (int depth = 0; !(depth == 0 && tok_equal(tok, ")")); tok = tok->next) {
    if (tok->at_bol || tok->kind == TK_EOF)
      error_tok(start, "unterminated embed parameter");
    depth += tok_equal(tok, "(") - tok_equal(tok, ")");
    cur = cur->next = tok_copy(tok);
  }
  cur->next = tok_new_eof(tok);
  *rest = tok->next;
  return head.next;
}

static Token *read_embed_params(Token *tok, EmbedParams *p, bool stop_at_paren) {
  *p = (EmbedParams){.limit = -1};
  while (!tok->at_bol && tok->kind != TK_EOF && !(stop_at_paren && tok_equal(tok, ")"))) {
    Token *name = tok;
    // Accept both "limit" and the reserved "__limit__" spellings.
    char *n = tok_text(name);
    size_t len = strlen(n);
    if (len > 4 && starts_with(n, "__") && ends_with(n, "__"))
      n = xstrndup(n + 2, len - 4);
    tok = tok->next;
    if (strcmp(n, "limit") == 0) {
      Token *arg = pp_expand(read_paren_group(&tok, tok));
      if (arg->kind != TK_PP_NUM || !convert_pp_int(arg) || arg->ival < 0)
        error_tok(name, "limit() requires a non-negative integer constant");
      p->limit = (long)arg->ival;
    } else if (strcmp(n, "prefix") == 0) {
      p->prefix = read_paren_group(&tok, tok);
    } else if (strcmp(n, "suffix") == 0) {
      p->suffix = read_paren_group(&tok, tok);
    } else if (strcmp(n, "if_empty") == 0) {
      p->if_empty = read_paren_group(&tok, tok);
    } else {
      error_tok(name, "unknown #embed parameter '%s'", n);
    }
  }
  return tok;
}

static char *find_embed(Token **rest, Token *tok) {
  bool is_quote;
  char *name = read_header_name(rest, tok, &is_quote, HDR_LITERAL);
  int idx;
  return search_include(name, is_quote, tok, -1, &idx);
}

bool pp_has_embed_resource(Token *tok, Token **rest, int *status) {
  tok = tok_skip(tok, "(");
  char *path = find_embed(&tok, tok);
  EmbedParams params;
  tok = read_embed_params(tok, &params, true);
  *rest = tok_skip(tok, ")");
  if (!path) {
    *status = 0;
  } else {
    size_t len = 0;
    char *data = read_file(path, &len);
    *status = (!data || len == 0 || params.limit == 0) ? 2 : 1;
  }
  return path != nullptr;
}

static Token *embed_directive(Token *directive, Token *tok, Token **rest) {
  char *path = find_embed(&tok, tok);
  if (!path)
    error_tok(directive, "#embed resource not found");
  EmbedParams p;
  tok = read_embed_params(tok, &p, false);
  *rest = tok;

  size_t len = 0;
  unsigned char *data = (unsigned char *)read_file(path, &len);
  if (!data)
    error_tok(directive, "cannot read '%s'", path);
  if (p.limit >= 0 && (size_t)p.limit < len)
    len = (size_t)p.limit;

  if (len == 0)
    return p.if_empty ? p.if_empty : tok_new_eof(directive);

  StrBuf sb = {};
  for (size_t i = 0; i < len; i++)
    sb_printf(&sb, i ? ",%u" : "%u", data[i]);
  Token *body = pp_tokenize_text(sb.data, directive);
  for (Token *t = body; t; t = t->next)
    t->at_bol = false;

  Token *result = body;
  if (p.prefix)
    result = pp_append(p.prefix, result);
  if (p.suffix) {
    Token *t = result;
    while (t->next->kind != TK_EOF)
      t = t->next;
    t->next = p.suffix;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Conditional inclusion
// ---------------------------------------------------------------------------

static CondIncl *push_cond(Token *tok, bool included) {
  CondIncl *ci = NEW(CondIncl);
  ci->next = pp.cond;
  ci->ctx = IN_THEN;
  ci->tok = tok;
  ci->included = included;
  pp.cond = ci;
  return ci;
}

static bool is_hash(const Token *tok) { return tok->at_bol && tok_equal(tok, "#"); }

// Skips a nested #if ... #endif block.
static Token *skip_cond_nested(Token *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && (tok_equal(tok->next, "if") || tok_equal(tok->next, "ifdef") ||
                         tok_equal(tok->next, "ifndef"))) {
      tok = skip_cond_nested(tok->next->next);
      continue;
    }
    if (is_hash(tok) && tok_equal(tok->next, "endif"))
      return tok->next->next;
    tok = tok->next;
  }
  return tok;
}

// Skips to the next #elif/#else/#endif of the current level.
static Token *skip_cond(Token *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && (tok_equal(tok->next, "if") || tok_equal(tok->next, "ifdef") ||
                         tok_equal(tok->next, "ifndef"))) {
      tok = skip_cond_nested(tok->next->next);
      continue;
    }
    if (is_hash(tok) &&
        (tok_equal(tok->next, "elif") || tok_equal(tok->next, "elifdef") ||
         tok_equal(tok->next, "elifndef") || tok_equal(tok->next, "else") ||
         tok_equal(tok->next, "endif")))
      return tok;
    tok = tok->next;
  }
  return tok;
}

static bool eval_defined_directive(Token **rest, Token *tok, bool negate) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "macro name must be an identifier");
  bool defined = macro_is_defined(tok);
  *rest = skip_line(tok->next);
  return negate ? !defined : defined;
}

// ---------------------------------------------------------------------------
// Other directives
// ---------------------------------------------------------------------------

static Token *line_directive(Token *tok) {
  Token *start = tok;
  Token *line = pp_expand(pp_copy_line(&tok, tok));
  if (line->kind != TK_PP_NUM || !convert_pp_int(line))
    error_tok(start, "#line directive requires a positive integer argument");
  int delta = (int)line->ival - (start->line + 1);
  char *name = nullptr;
  if (line->next->kind == TK_STR)
    name = line->next->str;

  // Renumber the rest of this file.
  for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
    if (t->file != start->file)
      continue;
    t->line += delta;
  }
  if (name) {
    start->file->display_name = xstrdup(name);
  }
  return tok;
}

// #pragma pack(N), pack(), pack(push[, N]), pack(pop): the maximum
// alignment of struct members declared after it (GCC and MSVC semantics).
static Token *pragma_pack(Token *tok) {
  Token *start = tok;
  tok = tok_skip(tok->next, "(");
  int value = -1; // -1: unchanged
  bool push = false, pop = false;
  for (bool first = true; !tok_equal(tok, ")"); first = false) {
    if (!first)
      tok = tok_skip(tok, ",");
    if (tok_equal(tok, "push") || tok_equal(tok, "pop")) {
      push = tok_equal(tok, "push");
      pop = !push;
      tok = tok->next;
    } else if (tok->kind == TK_PP_NUM && convert_pp_int(tok)) {
      value = (int)tok->ival;
      if (value < 1 || value > 16 || (value & (value - 1)))
        error_tok(tok, "alignment in '#pragma pack' must be 1, 2, 4, 8 or 16");
      tok = tok->next;
    } else if (tok->kind == TK_IDENT) {
      tok = tok->next; // MSVC's named push/pop label
    } else {
      error_tok(tok, "malformed '#pragma pack'");
    }
  }
  tok = tok->next;
  if (push) {
    if (pp.pack_depth == (int)ARRAY_LEN(pp.pack_stack))
      error_tok(start, "'#pragma pack(push)' nested too deeply");
    pp.pack_stack[pp.pack_depth++] = pp.pack;
  } else if (pop) {
    if (pp.pack_depth == 0)
      warn_tok(W_CPP, start, "'#pragma pack(pop)' without a matching push");
    else
      pp.pack = pp.pack_stack[--pp.pack_depth];
  }
  if (value >= 0)
    pp.pack = value;
  else if (!push && !pop)
    pp.pack = 0; // pack() restores natural alignment
  return tok;
}

static Token *pragma_directive(Token *directive, Token *tok) {
  if (tok_equal(tok, "pack") && tok_equal(tok->next, "(")) {
    tok = pragma_pack(tok);
    while (!tok->at_bol)
      tok = tok->next;
    return tok;
  }
  if (tok_equal(tok, "once") && tok->next->at_bol) {
    hashmap_put(&pp.pragma_once, canonical_path(directive->file->path), (void *)1);
    return tok->next;
  }
  if (tok_equal(tok, "GCC") && tok_equal(tok->next, "system_header")) {
    directive->file->is_system = true;
  }
  // Unknown pragmas are ignored, as the standard permits.
  while (!tok->at_bol)
    tok = tok->next;
  return tok;
}

static Token *message_directive(Token *directive, Token *tok, bool is_error) {
  Token *msg = pp_copy_line(&tok, tok);
  char *text = format("#%.*s %s", directive->len, directive->loc, "");
  StrBuf sb = {};
  sb_puts(&sb, text);
  for (Token *t = msg; t->kind != TK_EOF; t = t->next) {
    if (t != msg && t->has_space)
      sb_putc(&sb, ' ');
    sb_append(&sb, t->loc, (size_t)t->len);
  }
  if (is_error)
    error_tok(directive, "%s", sb.data);
  warn_tok(W_CPP, directive, "%s", sb.data);
  return tok;
}

// Executes and removes a _Pragma("...") operator (C99 6.10.9); only
// _Pragma("pack(...)") has an effect.
static Token *pragma_operator(Token *tok) {
  Token *start = tok;
  tok = tok_skip(tok->next, "(");
  if (tok->kind != TK_STR)
    error_tok(start, "_Pragma takes a parenthesized string literal");
  if (tok->str && starts_with(tok->str, "pack")) {
    Token *line = pp_tokenize_text(tok->str, tok);
    if (tok_equal(line, "pack") && tok_equal(line->next, "("))
      pragma_pack(line);
  }
  return tok_skip(tok->next, ")");
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

static Token *preprocess2(Token *tok) {
  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    if (macro_try_expand(&tok, tok))
      continue;

    if (tok->kind == TK_IDENT && tok_equal(tok, "_Pragma") && tok_equal(tok->next, "(")) {
      tok = pragma_operator(tok);
      continue;
    }

    if (!is_hash(tok)) {
      tok->pack = (unsigned char)pp.pack;
      cur = cur->next = tok;
      tok = tok->next;
      continue;
    }

    Token *start = tok;
    tok = tok->next;

    if (tok->at_bol) // null directive
      continue;

    if (tok_equal(tok, "include") || tok_equal(tok, "include_next")) {
      bool next = tok_equal(tok, "include_next");
      bool is_quote;
      Token *directive = tok;
      char *name = read_header_name(&tok, tok->next, &is_quote, HDR_LINE);
      tok = skip_line(tok);
      int start_index = -1;
      if (next)
        start_index = directive->file->include_index >= 0 ? directive->file->include_index + 1 : 0;
      int idx;
      char *path = search_include(name, is_quote, directive, start_index, &idx);
      if (!path)
        error_tok(directive->next, "'%s' file not found", name);
      tok = include_file(tok, path, idx, directive->next);
      continue;
    }

    if (tok_equal(tok, "embed")) {
      Token *body = embed_directive(tok, tok->next, &tok);
      tok = skip_line(tok);
      tok = pp_append(body, tok);
      continue;
    }

    if (tok_equal(tok, "define")) {
      tok = macro_define(tok->next);
      continue;
    }

    if (tok_equal(tok, "undef")) {
      tok = tok->next;
      if (tok->kind != TK_IDENT)
        error_tok(tok, "macro name must be an identifier");
      macro_undef(tok);
      tok = skip_line(tok->next);
      continue;
    }

    if (tok_equal(tok, "if")) {
      Token *directive = tok;
      bool val = pp_eval_condition(pp_copy_line(&tok, tok->next), directive);
      push_cond(start, val);
      if (!val)
        tok = skip_cond(tok);
      continue;
    }

    if (tok_equal(tok, "ifdef") || tok_equal(tok, "ifndef")) {
      bool val = eval_defined_directive(&tok, tok->next, tok_equal(tok, "ifndef"));
      push_cond(start, val);
      if (!val)
        tok = skip_cond(tok);
      continue;
    }

    if (tok_equal(tok, "elif") || tok_equal(tok, "elifdef") || tok_equal(tok, "elifndef")) {
      Token *directive = tok;
      if (!pp.cond || pp.cond->ctx == IN_ELSE)
        error_tok(start, "#%.*s without #if", directive->len, directive->loc);
      pp.cond->ctx = IN_ELIF;
      if (pp.cond->included) {
        while (!tok->next->at_bol)
          tok = tok->next;
        tok = skip_cond(tok->next);
        continue;
      }
      bool val;
      if (tok_equal(directive, "elif"))
        val = pp_eval_condition(pp_copy_line(&tok, tok->next), directive);
      else
        val = eval_defined_directive(&tok, tok->next, tok_equal(directive, "elifndef"));
      pp.cond->included = val;
      if (!val)
        tok = skip_cond(tok);
      continue;
    }

    if (tok_equal(tok, "else")) {
      if (!pp.cond || pp.cond->ctx == IN_ELSE)
        error_tok(start, "#else without #if");
      pp.cond->ctx = IN_ELSE;
      tok = skip_line(tok->next);
      if (pp.cond->included)
        tok = skip_cond(tok);
      pp.cond->included = true;
      continue;
    }

    if (tok_equal(tok, "endif")) {
      if (!pp.cond)
        error_tok(start, "#endif without #if");
      pp.cond = pp.cond->next;
      tok = skip_line(tok->next);
      continue;
    }

    if (tok_equal(tok, "line")) {
      tok = line_directive(tok->next);
      continue;
    }

    if (tok->kind == TK_PP_NUM) { // GNU line marker: # 42 "file"
      tok = line_directive(tok);
      continue;
    }

    if (tok_equal(tok, "pragma")) {
      tok = pragma_directive(tok, tok->next);
      continue;
    }

    if (tok_equal(tok, "error") || tok_equal(tok, "warning")) {
      tok = message_directive(tok, tok->next, tok_equal(tok, "error"));
      continue;
    }

    if (tok_equal(tok, "ident") || tok_equal(tok, "sccs")) {
      while (!tok->at_bol)
        tok = tok->next;
      continue;
    }

    error_tok(tok, "invalid preprocessing directive '#%.*s'", tok->len, tok->loc);
  }

  cur->next = tok;
  return head.next;
}

// ---------------------------------------------------------------------------
// Translation phase 6 and entry points
// ---------------------------------------------------------------------------

// Concatenates adjacent string literals, converting to a common encoding.
static void join_adjacent_strings(Token *tok) {
  for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
    if (t->kind != TK_STR || t->next->kind != TK_STR)
      continue;

    StrEncoding enc = t->enc;
    for (Token *u = t; u->kind == TK_STR; u = u->next) {
      if (u->enc == ENC_CHAR)
        continue;
      if (enc != ENC_CHAR && enc != u->enc)
        error_tok(u, "unsupported non-standard concatenation of string literals");
      enc = u->enc;
    }

    // Re-read each piece in the common encoding by re-lexing its body.
    int elem = encoding_elem_size(enc);
    StrBuf data = {};
    int count = 0;
    Token *end = t;
    for (Token *u = t; u->kind == TK_STR; u = u->next) {
      Token *piece = u;
      if (u->enc != enc) {
        // Re-lex with the common prefix so escapes and UTF-8 decode correctly.
        const char *quote = memchr(u->loc, '"', (size_t)u->len);
        const char *prefix = enc == ENC_UTF8 ? "u8" : enc == ENC_UTF16 ? "u" : enc == ENC_UTF32 ? "U" : "L";
        char *text = format("%s%.*s", prefix, (int)(u->loc + u->len - quote), quote);
        piece = pp_tokenize_text(text, u);
      }
      sb_append(&data, piece->str, (size_t)(piece->str_len - 1) * (size_t)elem);
      count += piece->str_len - 1;
      end = u;
    }
    for (int i = 0; i < elem; i++)
      sb_putc(&data, '\0');

    t->enc = enc;
    t->str = data.data;
    t->str_len = count + 1;
    t->next = end->next;
  }
}

void preproc_finalize(Token *tok) {
  convert_pp_tokens(tok);
  join_adjacent_strings(tok);
}

static char *get_base_file(void) { return pp.base_file; }

void preproc_init(const PreprocOptions *opts) {
  pp = (typeof(pp)){};
  pp_stats = (PreprocStats){};

  for (size_t i = 0; i < opts->quote_dirs.len; i++)
    vec_push(&pp.dirs, ((IncludeDir){opts->quote_dirs.data[i], false}));
  pp.first_angle_dir = (int)pp.dirs.len;
  for (size_t i = 0; i < opts->user_dirs.len; i++)
    vec_push(&pp.dirs, ((IncludeDir){opts->user_dirs.data[i], false}));
  for (size_t i = 0; i < opts->system_dirs.len; i++)
    vec_push(&pp.dirs, ((IncludeDir){opts->system_dirs.data[i], true}));

  macro_reset(opts);
  macro_set_file_hooks(get_base_file);
}

Token *preprocess_file(const char *path, PreprocStats *stats) {
  pp.base_file = xstrdup(path);
  SourceFile *file = open_source(path);
  if (!file)
    fatal("cannot open '%s'", path);

  Token *tok = preprocess2(tokenize(file));
  if (pp.cond)
    error_tok(pp.cond->tok, "unterminated conditional directive");

  for (Token *t = tok; t->kind != TK_EOF; t = t->next)
    pp_stats.tokens++;
  if (stats)
    *stats = pp_stats;
  return tok;
}

void print_tokens(FILE *out, Token *tok) {
  SourceFile *last_file = nullptr;
  int line = 1;
  for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
    // Position output by the expansion site, not by where macros were defined.
    const Token *site = t;
    while (site->origin)
      site = site->origin;
    if (site->file != last_file && site->file) {
      if (last_file)
        fputc('\n', out);
      fprintf(out, "# %d \"%s\"\n", site->line, site->file->display_name);
      last_file = site->file;
      line = site->line;
    } else if (t->at_bol && site->line != line) {
      if (site->line > line && site->line - line < 8) {
        for (; line < site->line; line++)
          fputc('\n', out);
      } else {
        fprintf(out, "\n# %d \"%s\"\n", site->line, site->file->display_name);
      }
      line = site->line;
    } else if (t->has_space || t->at_bol) {
      fputc(' ', out);
    }
    fprintf(out, "%.*s", t->len, t->loc);
  }
  fputc('\n', out);
}

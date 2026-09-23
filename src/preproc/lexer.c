// lexer.c - converts source text into preprocessing tokens.
#include "preproc/token.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>

#include "support/hashmap.h"
#include "support/strbuf.h"

// ---------------------------------------------------------------------------
// Token helpers
// ---------------------------------------------------------------------------

bool tok_equal(const Token *tok, const char *s) {
  return (tok->kind != TK_STR && tok->kind != TK_EOF) && (size_t)tok->len == strlen(s) &&
         memcmp(tok->loc, s, (size_t)tok->len) == 0;
}

Token *tok_skip(Token *tok, const char *s) {
  if (!tok_equal(tok, s))
    error_tok(tok, "expected '%s'", s);
  return tok->next;
}

bool tok_consume(Token **rest, Token *tok, const char *s) {
  if (tok_equal(tok, s)) {
    *rest = tok->next;
    return true;
  }
  *rest = tok;
  return false;
}

char *tok_text(const Token *tok) { return xstrndup(tok->loc, (size_t)tok->len); }

Token *tok_copy(const Token *tok) {
  Token *t = NEW(Token);
  *t = *tok;
  t->next = nullptr;
  return t;
}

Token *tok_new_eof(const Token *tok) {
  Token *t = tok_copy(tok);
  t->kind = TK_EOF;
  t->len = 0;
  return t;
}

SrcLoc tok_loc(const Token *tok) {
  return (SrcLoc){.file = tok->file, .pos = tok->loc, .len = tok->len, .line = tok->line};
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

static const Token *expansion_site(const Token *tok) {
  while (tok->origin)
    tok = tok->origin;
  return tok;
}

static void report_tok(DiagLevel level, WarningId id, const Token *tok, const char *fmt, va_list ap) {
  // Warnings about code spelled inside system headers (including macros
  // defined there) are not the user's problem.
  if (level == DIAG_WARNING && tok->file && tok->file->is_system)
    return;
  const Token *site = expansion_site(tok);
  SrcLoc loc = tok_loc(site);
  if (!diag_vreport(level, id, tok->file ? &loc : nullptr, fmt, ap))
    return;
  if (site != tok && tok->origin && tok->file && !tok->file->is_system) {
    SrcLoc body = tok_loc(tok);
    diag_report(DIAG_NOTE, W_NONE, &body, "in expansion of macro '%.*s'", tok->origin->len,
                tok->origin->loc);
  }
}

void error_tok(const Token *tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_tok(DIAG_ERROR, W_NONE, tok, fmt, ap);
  va_end(ap);
  diag_abort();
}

void error_tok_nofatal(const Token *tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_tok(DIAG_ERROR, W_NONE, tok, fmt, ap);
  va_end(ap);
}

void warn_tok(WarningId id, const Token *tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_tok(DIAG_WARNING, id, tok, fmt, ap);
  va_end(ap);
}

void note_tok(const Token *tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_tok(DIAG_NOTE, W_NONE, tok, fmt, ap);
  va_end(ap);
}

static int line_of(SourceFile *file, const char *pos) {
  int line = 1;
  for (const char *p = file->contents; p < pos; p++)
    if (*p == '\n')
      line++;
  return line;
}

void error_at(SourceFile *file, const char *pos, const char *fmt, ...) {
  SrcLoc loc = {.file = file, .pos = pos, .len = 1, .line = line_of(file, pos)};
  va_list ap;
  va_start(ap, fmt);
  diag_vreport(DIAG_ERROR, W_NONE, &loc, fmt, ap);
  va_end(ap);
  diag_abort();
}

// ---------------------------------------------------------------------------
// Keywords
// ---------------------------------------------------------------------------

static const char *const keywords[] = {
    // C23
    "alignas", "alignof", "auto", "bool", "break", "case", "char", "const", "constexpr",
    "continue", "default", "do", "double", "else", "enum", "extern", "false", "float", "for",
    "goto", "if", "inline", "int", "long", "nullptr", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "static_assert", "struct", "switch", "thread_local", "true",
    "typedef", "typeof", "typeof_unqual", "union", "unsigned", "void", "volatile", "while",
    "_Alignas", "_Alignof", "_Atomic", "_BitInt", "_Bool", "_Complex", "_Decimal128",
    "_Decimal32", "_Decimal64", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
    "_Thread_local",
    // Common GNU spellings, accepted for compatibility.
    "__alignof__", "__asm", "__asm__", "asm", "__attribute__", "__const", "__extension__",
    "__inline", "__inline__", "__restrict", "__restrict__", "__signed__", "__thread",
    "__typeof", "__typeof__", "__volatile__",
};

bool is_keyword(const Token *tok) {
  static HashMap map;
  if (!map.capacity)
    for (size_t i = 0; i < ARRAY_LEN(keywords); i++)
      hashmap_put(&map, keywords[i], (void *)1);
  return hashmap_get2(&map, tok->loc, tok->len);
}

// ---------------------------------------------------------------------------
// Character classes and punctuators
// ---------------------------------------------------------------------------

static bool is_ident_start(int c) { return isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
static bool is_ident_char(int c) { return is_ident_start(c) || isdigit(c); }

static int punct_len(const char *p) {
  static const char *const puncts[] = {
      "<<=", ">>=", "...", "==", "!=", "<=", ">=", "->", "+=", "-=", "*=", "/=", "++", "--",
      "%=",  "&=",  "|=",  "^=", "&&", "||", "<<", ">>", "##", "::",
  };
  for (size_t i = 0; i < ARRAY_LEN(puncts); i++)
    if (starts_with(p, puncts[i]))
      return (int)strlen(puncts[i]);
  return ispunct((unsigned char)*p) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Escape sequences and string literals
// ---------------------------------------------------------------------------

static int hex_value(int c) {
  if (isdigit(c))
    return c - '0';
  return tolower(c) - 'a' + 10;
}

typedef struct {
  SourceFile *file;
  bool multichar; // last character constant had several characters
} Lexer;

// Reads one escape sequence starting after the backslash. Returns the value;
// *is_ucn tells whether it was a universal character name (a code point).
static uint32_t read_escape(Lexer *lx, char **newpos, char *p, bool *is_ucn) {
  *is_ucn = false;
  if ('0' <= *p && *p <= '7') {
    uint32_t c = (uint32_t)(*p++ - '0');
    for (int i = 0; i < 2 && '0' <= *p && *p <= '7'; i++)
      c = (c << 3) + (uint32_t)(*p++ - '0');
    *newpos = p;
    return c;
  }
  if (*p == 'x') {
    p++;
    if (!isxdigit((unsigned char)*p))
      error_at(lx->file, p, "\\x used with no following hex digits");
    uint64_t c = 0;
    for (; isxdigit((unsigned char)*p); p++) {
      c = (c << 4) + (uint64_t)hex_value(*p);
      if (c > 0xFFFFFFFF)
        error_at(lx->file, p, "hex escape sequence out of range");
    }
    *newpos = p;
    return (uint32_t)c;
  }
  if (*p == 'u' || *p == 'U') {
    int n = *p == 'u' ? 4 : 8;
    char *start = p++;
    uint32_t c = 0;
    for (int i = 0; i < n; i++, p++) {
      if (!isxdigit((unsigned char)*p))
        error_at(lx->file, start, "incomplete universal character name");
      c = (c << 4) + (uint32_t)hex_value(*p);
    }
    *is_ucn = true;
    *newpos = p;
    return c;
  }

  *newpos = p + 1;
  switch (*p) {
  case 'a': return '\a';
  case 'b': return '\b';
  case 't': return '\t';
  case 'n': return '\n';
  case 'v': return '\v';
  case 'f': return '\f';
  case 'r': return '\r';
  case 'e': return 27; // GNU extension
  case '\\': case '\'': case '"': case '?':
    return (uint32_t)*p;
  default:
    error_at(lx->file, p - 1, "unknown escape sequence '\\%c'", *p);
  }
}

static void encode_utf8(StrBuf *sb, uint32_t c) {
  if (c < 0x80) {
    sb_putc(sb, (char)c);
  } else if (c < 0x800) {
    sb_putc(sb, (char)(0xC0 | (c >> 6)));
    sb_putc(sb, (char)(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    sb_putc(sb, (char)(0xE0 | (c >> 12)));
    sb_putc(sb, (char)(0x80 | ((c >> 6) & 0x3F)));
    sb_putc(sb, (char)(0x80 | (c & 0x3F)));
  } else {
    sb_putc(sb, (char)(0xF0 | (c >> 18)));
    sb_putc(sb, (char)(0x80 | ((c >> 12) & 0x3F)));
    sb_putc(sb, (char)(0x80 | ((c >> 6) & 0x3F)));
    sb_putc(sb, (char)(0x80 | (c & 0x3F)));
  }
}

// Decodes one UTF-8 sequence from the source (invalid bytes pass through).
static uint32_t decode_utf8(char **newpos, char *p) {
  unsigned char c = (unsigned char)*p;
  int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
  if (n == 1) {
    *newpos = p + 1;
    return c;
  }
  uint32_t cp = c & (0x7F >> n);
  for (int i = 1; i < n; i++) {
    if ((p[i] & 0xC0) != 0x80) {
      *newpos = p + 1;
      return c;
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *newpos = p + n;
  return cp;
}

int encoding_elem_size(StrEncoding enc) {
  switch (enc) {
  case ENC_CHAR: case ENC_UTF8: return 1;
  case ENC_UTF16: return 2;
  case ENC_UTF32: case ENC_WIDE: return 4;
  }
  return 1;
}

static void put_unit(StrBuf *sb, int size, uint32_t v) {
  for (int i = 0; i < size; i++)
    sb_putc(sb, (char)(v >> (8 * i)));
}

// Appends one code point/code unit to a literal buffer in the given encoding.
static int emit_char(StrBuf *sb, StrEncoding enc, uint32_t c, bool is_code_point) {
  switch (enc) {
  case ENC_CHAR:
  case ENC_UTF8:
    if (is_code_point) {
      size_t before = sb->len;
      encode_utf8(sb, c);
      return (int)(sb->len - before);
    }
    sb_putc(sb, (char)c);
    return 1;
  case ENC_UTF16:
    if (is_code_point && c >= 0x10000) {
      c -= 0x10000;
      put_unit(sb, 2, 0xD800 + (c >> 10));
      put_unit(sb, 2, 0xDC00 + (c & 0x3FF));
      return 2;
    }
    put_unit(sb, 2, c);
    return 1;
  case ENC_UTF32:
  case ENC_WIDE:
    put_unit(sb, 4, c);
    return 1;
  }
  return 0;
}

// Reads the body of a string/char literal starting at the opening quote.
// Returns the element count (without terminator) and the end position.
static int read_literal_body(Lexer *lx, StrBuf *sb, StrEncoding enc, char *quote, char **end) {
  char q = *quote;
  int count = 0;
  char *p = quote + 1;
  while (*p != q) {
    if (*p == '\n' || *p == '\0')
      error_at(lx->file, quote, "missing terminating %c character", q);
    uint32_t c;
    bool is_cp;
    if (*p == '\\') {
      c = read_escape(lx, &p, p + 1, &is_cp);
    } else if (enc == ENC_CHAR || enc == ENC_UTF8) {
      c = (unsigned char)*p++; // copy source bytes verbatim
      is_cp = false;
    } else {
      c = decode_utf8(&p, p);
      is_cp = true;
    }
    if (!is_cp && (enc == ENC_CHAR || enc == ENC_UTF8) && c > 0xFF)
      error_at(lx->file, p - 1, "escape sequence out of range");
    count += emit_char(sb, enc, c, is_cp);
  }
  *end = p + 1;
  return count;
}

static Token *new_token(Lexer *lx, TokenKind kind, char *start, char *end) {
  Token *tok = NEW(Token);
  tok->kind = kind;
  tok->loc = start;
  tok->len = (int)(end - start);
  tok->file = lx->file;
  return tok;
}

static Token *read_string(Lexer *lx, char *start, char *quote, StrEncoding enc) {
  StrBuf sb = {};
  char *end;
  int count = read_literal_body(lx, &sb, enc, quote, &end);
  put_unit(&sb, encoding_elem_size(enc), 0);
  Token *tok = new_token(lx, TK_STR, start, end);
  tok->enc = enc;
  tok->str = sb.data;
  tok->str_len = count + 1;
  return tok;
}

static Token *read_char(Lexer *lx, char *start, char *quote, StrEncoding enc) {
  StrBuf sb = {};
  char *end;
  int count = read_literal_body(lx, &sb, enc, quote, &end);
  if (count == 0)
    error_at(lx->file, start, "empty character constant");

  Token *tok = new_token(lx, TK_NUM, start, end);
  int size = encoding_elem_size(enc);
  const unsigned char *b = (const unsigned char *)sb.data;
  switch (enc) {
  case ENC_CHAR:
    if (count == 1) {
      tok->ival = (signed char)b[0]; // plain char is signed on x86-64
    } else {
      // Implementation-defined; follow GCC: big-endian packing into an int.
      uint32_t v = 0;
      for (int i = 0; i < count; i++)
        v = (v << 8) | b[i];
      tok->ival = (int32_t)v;
      lx->multichar = true;
    }
    tok->lit = LIT_INT;
    break;
  case ENC_UTF8:
    if (count != 1)
      error_at(lx->file, start, "character not encodable in a single code unit");
    tok->ival = b[0];
    tok->lit = LIT_UCHAR;
    break;
  default: {
    if (count != 1)
      error_at(lx->file, start, "character not encodable in a single code unit");
    uint32_t v = 0;
    for (int i = size - 1; i >= 0; i--)
      v = (v << 8) | b[i];
    tok->ival = enc == ENC_WIDE ? (int32_t)v : (int64_t)v;
    tok->lit = enc == ENC_UTF16 ? LIT_USHORT : enc == ENC_UTF32 ? LIT_UINT : LIT_INT;
  }
  }
  return tok;
}

// Recognizes an encoding prefix followed by a quote; returns prefix length.
static int literal_prefix(const char *p, char quote, StrEncoding *enc) {
  if (p[0] == quote) {
    *enc = ENC_CHAR;
    return 0;
  }
  if (p[0] == 'u' && p[1] == '8' && p[2] == quote) {
    *enc = ENC_UTF8;
    return 2;
  }
  if ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && p[1] == quote) {
    *enc = p[0] == 'u' ? ENC_UTF16 : p[0] == 'U' ? ENC_UTF32 : ENC_WIDE;
    return 1;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

// Copies a pp-number without C23 digit separators.
static char *strip_separators(const Token *tok) {
  char *buf = xmalloc((size_t)tok->len + 1);
  int n = 0;
  for (int i = 0; i < tok->len; i++)
    if (tok->loc[i] != '\'')
      buf[n++] = tok->loc[i];
  buf[n] = '\0';
  return buf;
}

bool convert_pp_int(Token *tok) {
  char *buf = strip_separators(tok);
  char *p = buf;

  int base = 10;
  if ((p[0] == '0') && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
    base = 16;
    p += 2;
  } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
    base = 2;
    p += 2;
  } else if (p[0] == '0') {
    base = 8;
  }

  uint64_t val = 0;
  bool overflow = false;
  for (;; p++) {
    int d;
    if (isdigit((unsigned char)*p))
      d = *p - '0';
    else if (base == 16 && isxdigit((unsigned char)*p))
      d = hex_value(*p);
    else
      break;
    if (d >= base)
      return false;
    if (val > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
      overflow = true;
    val = val * (uint64_t)base + (uint64_t)d;
  }

  // Integer suffix: any order of u and l/ll (ll must match case).
  bool u = false;
  int l = 0;
  while (*p) {
    if ((*p == 'u' || *p == 'U') && !u) {
      u = true;
      p++;
    } else if ((starts_with(p, "ll") || starts_with(p, "LL")) && !l) {
      l = 2;
      p += 2;
    } else if ((*p == 'l' || *p == 'L') && !l) {
      l = 1;
      p++;
    } else {
      return false;
    }
  }

  if (overflow)
    error_tok(tok, "integer constant is too large for its type");

  // Pick the first type that can represent the value (C23 6.4.4.1).
  bool any_base = base != 10;
  LiteralType lit;
  if (u) {
    lit = l == 2 ? LIT_ULLONG : (l == 1 || (val >> 32)) ? LIT_ULONG : LIT_UINT;
  } else if (l == 2) {
    lit = (val >> 63) ? LIT_ULLONG : LIT_LLONG;
  } else if (l == 1) {
    lit = (val >> 63) ? LIT_ULONG : LIT_LONG;
  } else if (val <= INT32_MAX) {
    lit = LIT_INT;
  } else if (any_base && val <= UINT32_MAX) {
    lit = LIT_UINT;
  } else if (val <= INT64_MAX) {
    lit = LIT_LONG;
  } else {
    lit = LIT_ULONG;
  }
  if (!u && !any_base && (val >> 63))
    warn_tok(W_OVERFLOW, tok, "integer constant is so large that it is unsigned");

  tok->kind = TK_NUM;
  tok->lit = lit;
  tok->ival = (int64_t)val;
  return true;
}

static void convert_pp_number(Token *tok) {
  if (convert_pp_int(tok))
    return;

  char *buf = strip_separators(tok);
  bool hex = buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X');
  bool looks_float = strchr(buf, '.') || (!hex && strpbrk(buf, "eE")) || (hex && strpbrk(buf, "pP"));
  if (!looks_float)
    error_tok(tok, "invalid numeric constant '%.*s'", tok->len, tok->loc);

  errno = 0;
  char *end;
  double val = strtod(buf, &end);
  LiteralType lit = LIT_DOUBLE;
  if (*end == 'f' || *end == 'F') {
    lit = LIT_FLOAT;
    end++;
  } else if (*end == 'l' || *end == 'L') {
    lit = LIT_LDOUBLE;
    end++;
  }
  if (*end)
    error_tok(tok, "invalid suffix '%s' on floating constant", end);
  if (errno == ERANGE && (val == HUGE_VAL || val == -HUGE_VAL))
    warn_tok(W_OVERFLOW, tok, "floating constant exceeds range of 'double'");

  tok->kind = TK_NUM;
  tok->lit = lit;
  tok->fval = lit == LIT_FLOAT ? (float)val : val;
}

void convert_pp_tokens(Token *tok) {
  for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT && is_keyword(t))
      t->kind = TK_KEYWORD;
    else if (t->kind == TK_PP_NUM)
      convert_pp_number(t);
  }
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

Token *tokenize(SourceFile *file) {
  Lexer lx = {.file = file};
  char *p = file->contents;
  Token head = {};
  Token *cur = &head;
  bool at_bol = true;
  bool has_space = false;
  int line = 1;

  while (*p) {
    if (starts_with(p, "//")) {
      while (*p && *p != '\n')
        p++;
      has_space = true;
      continue;
    }
    if (starts_with(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q)
        error_at(file, p, "unterminated comment");
      for (char *c = p; c < q; c++)
        if (*c == '\n')
          line++;
      p = q + 2;
      has_space = true;
      continue;
    }
    if (*p == '\n') {
      p++;
      line++;
      at_bol = true;
      has_space = false;
      continue;
    }
    if (isspace((unsigned char)*p)) {
      p++;
      has_space = true;
      continue;
    }

    Token *tok;
    StrEncoding enc;
    int n;
    if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
      char *q = p++;
      for (;;) {
        if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1]))
          p += 2;
        else if (*p == '\'' && is_ident_char((unsigned char)p[1]))
          p += 2;
        else if (isalnum((unsigned char)*p) || *p == '.' || *p == '_')
          p++;
        else
          break;
      }
      tok = new_token(&lx, TK_PP_NUM, q, p);
    } else if ((n = literal_prefix(p, '"', &enc)) >= 0) {
      tok = read_string(&lx, p, p + n, enc);
      p += tok->len;
    } else if ((n = literal_prefix(p, '\'', &enc)) >= 0) {
      lx.multichar = false;
      tok = read_char(&lx, p, p + n, enc);
      p += tok->len;
      if (lx.multichar) {
        tok->line = line;
        warn_tok(W_MULTICHAR, tok, "multi-character character constant");
      }
    } else if (is_ident_start((unsigned char)*p)) {
      char *q = p;
      while (is_ident_char((unsigned char)*p))
        p++;
      tok = new_token(&lx, TK_IDENT, q, p);
    } else if ((n = punct_len(p)) > 0) {
      tok = new_token(&lx, TK_PUNCT, p, p + n);
      p += n;
    } else {
      error_at(file, p, "invalid character in source");
    }

    tok->line = line;
    tok->at_bol = at_bol;
    tok->has_space = has_space;
    at_bol = has_space = false;
    cur = cur->next = tok;
  }

  Token *eof = new_token(&lx, TK_EOF, p, p);
  eof->line = line;
  eof->at_bol = true;
  cur->next = eof;
  return head.next;
}

// Normalizes line endings and splices backslash-newline sequences. Removed
// newlines are re-inserted after the logical line so line numbers stay exact.
static char *splice_lines(char *src) {
  size_t n = strlen(src);
  char *out = xmalloc(n + 2);
  size_t i = 0, j = 0;
  int pending = 0;
  while (i < n) {
    if (src[i] == '\r' && src[i + 1] == '\n') {
      i++;
      continue;
    }
    if (src[i] == '\\' && (src[i + 1] == '\n' || (src[i + 1] == '\r' && src[i + 2] == '\n'))) {
      i += src[i + 1] == '\n' ? 2 : 3;
      pending++;
      continue;
    }
    if (src[i] == '\n') {
      out[j++] = src[i++];
      for (; pending > 0; pending--)
        out[j++] = '\n';
      continue;
    }
    out[j++] = src[i++];
  }
  // Every file ends with a newline (makes directive handling uniform).
  if (j == 0 || out[j - 1] != '\n')
    out[j++] = '\n';
  for (; pending > 0; pending--)
    out[j++] = '\n';
  out[j] = '\0';
  return out;
}

SourceFile *load_source(const char *path) {
  char *raw = read_file(path, nullptr);
  if (!raw)
    return nullptr;
  // Skip a UTF-8 byte order mark.
  char *text = starts_with(raw, "\xEF\xBB\xBF") ? raw + 3 : raw;
  return source_new(path, splice_lines(text));
}

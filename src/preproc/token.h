// token.h - tokens produced by the lexer and consumed by the preprocessor/parser.
#pragma once

#include "support/diag.h"

typedef struct Hideset Hideset;

typedef enum : uint8_t {
  TK_IDENT,   // identifier
  TK_PUNCT,   // punctuator
  TK_KEYWORD, // keyword (only after preprocessing)
  TK_STR,     // string literal
  TK_NUM,     // numeric or character constant (only after preprocessing)
  TK_PP_NUM,  // preprocessing number
  TK_EOF,
} TokenKind;

// Type of a numeric/character literal, decided by the lexer.
typedef enum : uint8_t {
  LIT_INT,
  LIT_UINT,
  LIT_LONG,
  LIT_ULONG,
  LIT_LLONG,
  LIT_ULLONG,
  LIT_UCHAR,  // u8'x' (char8_t)
  LIT_USHORT, // u'x'  (char16_t)
  LIT_FLOAT,
  LIT_DOUBLE,
  LIT_LDOUBLE,
} LiteralType;

// Encoding prefix of a string literal.
typedef enum : uint8_t {
  ENC_CHAR,  // "..."
  ENC_UTF8,  // u8"..." (char8_t = unsigned char in C23)
  ENC_UTF16, // u"..."
  ENC_UTF32, // U"..."
  ENC_WIDE,  // L"..." (wchar_t = int on Linux)
} StrEncoding;

typedef struct Token Token;
struct Token {
  TokenKind kind;
  bool at_bol;    // first token on its line
  bool has_space; // preceded by whitespace
  int len;
  char *loc; // spelling, points into the source buffer
  Token *next;
  SourceFile *file;
  int line;

  // Literals.
  LiteralType lit;
  int64_t ival;
  double fval;
  StrEncoding enc;
  char *str;   // decoded string literal in target encoding, NUL-terminated
  int str_len; // number of elements including the terminator

  // Preprocessor bookkeeping.
  unsigned char pack; // #pragma pack in effect here: maximum member alignment (0 = none)
  Hideset *hideset;
  Token *origin; // macro name token this token was expanded from
};

// Lexing.
Token *tokenize(SourceFile *file);
SourceFile *load_source(const char *path); // nullptr if unreadable
void convert_pp_tokens(Token *tok);         // keywords and numbers, after preprocessing
bool convert_pp_int(Token *tok);            // integer pp-number -> TK_NUM (false if malformed)
int encoding_elem_size(StrEncoding enc);

// Token helpers.
bool tok_equal(const Token *tok, const char *s);
Token *tok_skip(Token *tok, const char *s);
bool tok_consume(Token **rest, Token *tok, const char *s);
char *tok_text(const Token *tok);
Token *tok_copy(const Token *tok);
Token *tok_new_eof(const Token *tok);
bool is_keyword(const Token *tok);
SrcLoc tok_loc(const Token *tok);

// Diagnostics anchored at tokens; macro expansions report the invocation site.
[[noreturn, gnu::format(printf, 2, 3)]] void error_tok(const Token *tok, const char *fmt, ...);
[[gnu::format(printf, 2, 3)]] void error_tok_nofatal(const Token *tok, const char *fmt, ...);
[[gnu::format(printf, 3, 4)]] void warn_tok(WarningId id, const Token *tok, const char *fmt, ...);
[[gnu::format(printf, 2, 3)]] void note_tok(const Token *tok, const char *fmt, ...);
[[noreturn, gnu::format(printf, 3, 4)]] void error_at(SourceFile *file, const char *pos, const char *fmt,
                                                     ...);

// Strings, character handling and literal encodings.
#include <ctype.h>
#include <stdint.h>
#include <uchar.h>
#include <wchar.h>

#include "test.h"

static size_t my_strlen(const char *s) {
  const char *p = s;
  while (*p)
    p++;
  return (size_t)(p - s);
}

static void reverse(char *s) {
  size_t n = strlen(s);
  for (size_t i = 0; i < n / 2; i++) {
    char t = s[i];
    s[i] = s[n - 1 - i];
    s[n - 1 - i] = t;
  }
}

int main(void) {
  ASSERT(5, my_strlen("hello"));
  ASSERT(0, my_strlen(""));
  ASSERT(6, sizeof "hello");
  ASSERT('e', "hello"[1]);
  ASSERT(0, "hello"[5]);

  // Escapes.
  ASSERT(7, "\a"[0]);
  ASSERT(8, "\b"[0]);
  ASSERT(9, "\t"[0]);
  ASSERT(10, "\n"[0]);
  ASSERT(11, "\v"[0]);
  ASSERT(12, "\f"[0]);
  ASSERT(13, "\r"[0]);
  ASSERT(27, "\e"[0]);
  ASSERT(92, "\\"[0]);
  ASSERT(34, "\""[0]);
  ASSERT(39, "\'"[0]);
  ASSERT(63, "\?"[0]);
  ASSERT(0x7f, "\x7f"[0]);
  ASSERT(-1, "\377"[0]);
  ASSERT(3, sizeof "\x41\x42");
  ASSERT('A', "\101"[0]);

  // Concatenation.
  ASSERT_STR("foobar", "foo" "bar");
  ASSERT(7, sizeof("foo" "bar"));
  const char *multi = "line1\n"
                      "line2";
  ASSERT(11, (int)strlen(multi));

  // UTF-8 source text and universal character names.
  const char *utf8 = "žluťoučký kůň";
  ASSERT(19, (int)strlen(utf8));
  ASSERT(2, (int)strlen("ž")); // ž
  ASSERT(4, (int)strlen("\U0001F98A")); // fox face

  // Wide and Unicode literals.
  const wchar_t *w = L"wide ž";
  ASSERT(6, (int)wcslen(w));
  ASSERT(0x17e, w[5]);
  ASSERT(4, sizeof L"ab" / sizeof(wchar_t) + 1);
  const char16_t *u16 = u"a\U0001F98A";
  ASSERT(0xD83E, u16[1]); // surrogate pair
  ASSERT(0xDD8A, u16[2]);
  const char32_t *u32 = U"\U0001F98A!";
  ASSERT(0x1F98A, u32[0]);
  ASSERT(8, sizeof U"a");
  ASSERT(1, sizeof(u8'a'));
  ASSERT(2, sizeof(u'a'));
  ASSERT(4, sizeof(U'a'));
  ASSERT(4, sizeof('a'));
  ASSERT(0x17e, L'ž');
  ASSERT(4, sizeof u8"ab" + 1);
  ASSERT('x', u8"x"[0]);

  // Mutable arrays initialized from literals.
  char word[] = "stressed";
  reverse(word);
  ASSERT_STR("desserts", word);

  // <string.h> and <ctype.h>.
  char buf[32];
  strcpy(buf, "abc");
  strcat(buf, "def");
  ASSERT_STR("abcdef", buf);
  ASSERT(0, strcmp("abc", "abc"));
  ASSERT(1, strcmp("abd", "abc") > 0);
  ASSERT(1, strncmp("abcx", "abcy", 3) == 0);
  ASSERT_STR("def", strstr(buf, "de"));
  ASSERT_STR("cdef", strchr(buf, 'c'));
  memset(buf, 'z', 3);
  ASSERT_STR("zzzdef", buf);
  ASSERT(1, !!isdigit('7'));
  ASSERT(0, !!isdigit('x'));
  ASSERT('Q', toupper('q'));
  ASSERT(1, !!isspace(' '));

  // Character arithmetic.
  int counts[26] = {};
  for (const char *p = "hello world"; *p; p++)
    if (islower((unsigned char)*p))
      counts[*p - 'a']++;
  ASSERT(3, counts['l' - 'a']);
  ASSERT(2, counts['o' - 'a']);

  // snprintf and sscanf.
  snprintf(buf, sizeof buf, "%d-%s-%c-%x-%05.1f", 12, "ab", 'z', 255, 3.14159);
  ASSERT_STR("12-ab-z-ff-003.1", buf);
  int n1, n2;
  char word2[16];
  ASSERT(3, sscanf("42 17 word", "%d %d %15s", &n1, &n2, word2));
  ASSERT(59, n1 + n2);
  ASSERT_STR("word", word2);

  return test_done();
}

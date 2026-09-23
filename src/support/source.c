// source.c - loading source files into memory.
#include "support/source.h"

#include "support/strbuf.h"

static int next_file_id;

SourceFile *source_new(const char *path, char *contents) {
  SourceFile *f = NEW(SourceFile);
  f->path = xstrdup(path);
  f->display_name = f->path;
  f->contents = contents;
  f->id = next_file_id++;
  f->include_index = -1;
  return f;
}

int source_count(void) { return next_file_id; }

char *read_file(const char *path, size_t *out_len) {
  FILE *fp = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
  if (!fp)
    return nullptr;

  StrBuf sb = {};
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
    sb_append(&sb, buf, n);
  bool failed = ferror(fp);
  if (fp != stdin)
    fclose(fp);
  if (failed)
    return nullptr;

  if (out_len)
    *out_len = sb.len;
  if (!sb.data)
    return xstrdup("");
  return sb.data;
}

// data.c - global variables, string literals and constants.
#include "codegen/cg.h"
#include "support/strbuf.h"

static const char *section_of(Obj *var) {
  if (var->is_tls)
    return var->init_data ? "\t.section .tdata,\"awT\",@progbits" : "\t.section .tbss,\"awT\",@nobits";
  if (!var->init_data)
    return "\t.bss";
  bool read_only = var->is_string_literal || var->ty->is_const ||
                   (var->ty->kind == TY_ARRAY && var->ty->base->is_const);
  if (!read_only)
    return "\t.data";
  // Constant data containing addresses must be relocated at load time;
  // .data.rel.ro becomes read-only after relocation (RELRO).
  return var->rel ? "\t.section .data.rel.ro,\"aw\"" : "\t.section .rodata";
}

static void emit_ascii(const unsigned char *p, int n) {
  StrBuf sb = {};
  sb_puts(&sb, "\t.ascii \"");
  for (int i = 0; i < n; i++) {
    unsigned char c = p[i];
    if (c == '"' || c == '\\')
      sb_printf(&sb, "\\%c", c);
    else if (c >= 0x20 && c < 0x7f)
      sb_putc(&sb, (char)c);
    else
      sb_printf(&sb, "\\%03o", c);
  }
  sb_putc(&sb, '"');
  emit_raw("%s", sb.data);
  free(sb.data);
}

static void emit_bytes(const unsigned char *p, int n) {
  int i = 0;
  while (i < n) {
    // Long runs of zeros.
    int z = i;
    while (z < n && p[z] == 0)
      z++;
    if (z - i >= 16) {
      emit_raw("\t.zero %d", z - i);
      i = z;
      continue;
    }
    StrBuf sb = {};
    sb_puts(&sb, "\t.byte ");
    for (int k = 0; k < 16 && i < n; k++, i++)
      sb_printf(&sb, k ? ",%u" : "%u", p[i]);
    emit_raw("%s", sb.data);
    free(sb.data);
  }
}

static void emit_var(Obj *var) {
  int size = var->ty->size;
  int align = var->align;
  if (var->ty->kind == TY_ARRAY && size >= 16)
    align = MAX(align, 16);

  emit_raw("%s", section_of(var));
  if (!var->is_static)
    emit_raw("\t.globl %s", var->name);
  emit_raw("\t.type %s, @%s", var->name, var->is_tls ? "tls_object" : "object");
  emit_raw("\t.size %s, %d", var->name, size);
  emit_raw("\t.p2align %d", log2_u64((uint64_t)MAX(align, 1)));
  emit_label("%s", var->name);

  if (!var->init_data) {
    emit_raw("\t.zero %d", MAX(size, 1));
    return;
  }

  const unsigned char *data = (const unsigned char *)var->init_data;
  if (var->is_string_literal && var->ty->base->size == 1 && !var->rel) {
    emit_ascii(data, size);
    return;
  }

  int pos = 0;
  for (Reloc *rel = var->rel; rel; rel = rel->next) {
    emit_bytes(data + pos, rel->offset - pos);
    if (rel->addend)
      emit_raw("\t.quad %s%+lld", rel->label, (long long)rel->addend);
    else
      emit_raw("\t.quad %s", rel->label);
    pos = rel->offset + 8;
  }
  emit_bytes(data + pos, size - pos);
}

void emit_data(Program *prog) {
  for (Obj *var = prog->globals; var; var = var->next)
    if (!var->is_function && var->is_definition)
      emit_var(var);
}

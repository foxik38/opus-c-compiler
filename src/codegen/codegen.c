// codegen.c - code generator entry point.
#include "codegen/cg.h"

static bool is_emitted_function(Obj *fn) {
  if (!fn->is_function || !fn->is_definition || !fn->body)
    return false;
  // Unreferenced static functions (typically static inline helpers from
  // headers) are not emitted.
  return !fn->is_static || fn->is_referenced;
}

void codegen(Program *prog, FILE *out, bool optimize, CodegenStats *stats) {
  cg = (CodegenState){.optimize = optimize, .stats = stats};

  for (Obj *fn = prog->globals; fn; fn = fn->next)
    if (is_emitted_function(fn))
      gen_function(fn);
  emit_data(prog);

  emit_raw("\t.ident \"occ " OCC_VERSION "\"");
  // The program never needs an executable stack.
  emit_raw("\t.section .note.GNU-stack,\"\",@progbits");

  if (optimize)
    stats->peephole_removed = peephole(&cg.lines);
  flush_lines(out);
}

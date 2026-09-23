// pipeline.c - drives the stages and reports on them.
//
// Internal stages (preprocess, parse, optimize, codegen) run in-process under
// a diagnostics recovery point; assemble and link run GNU as and ld as child
// processes. Each stage reports its CPU and wall-clock time.
#include "driver/pipeline.h"

#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "codegen/codegen.h"
#include "driver/report.h"
#include "driver/toolchain.h"
#include "opt/opt.h"
#include "parse/parse.h"
#include "preproc/preproc.h"
#include "support/diag.h"
#include "support/strbuf.h"

extern char **environ;

typedef struct {
  Options *opts;
  PreprocOptions pp;
  char *tmpdir;
  StrVec objects;
  Duration total;
  int tool_errors; // failures of external tools
} Build;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char *file_part(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static char *strip_extension(const char *path) {
  const char *name = file_part(path);
  const char *dot = strrchr(name, '.');
  return dot && dot != name ? xstrndup(name, (size_t)(dot - name)) : xstrdup(name);
}

static void add_time(Duration *acc, Duration d) {
  acc->cpu_ms += d.cpu_ms;
  acc->wall_ms += d.wall_ms;
}

static const char *plural(int n, const char *one, const char *many) { return n == 1 ? one : many; }

typedef void StageFn(void *ctx);

// Runs an in-process stage; fatal diagnostics unwind back here.
static bool guarded(StageFn *fn, void *ctx, Duration *time) {
  int errors_before = diag_error_count();
  jmp_buf jb;
  Stopwatch sw = stopwatch_start();
  bool ok = false;
  diag_set_recovery(&jb);
  if (setjmp(jb) == 0) {
    fn(ctx);
    ok = true;
  }
  diag_set_recovery(nullptr);
  *time = stopwatch_elapsed(&sw);
  return ok && diag_error_count() == errors_before;
}

static char *failure_detail(int errors_before) {
  int n = diag_error_count() - errors_before;
  return format("%d %s", n, plural(n, "error", "errors"));
}

static StageStatus status_of(bool ok, int warnings_before) {
  if (!ok)
    return STATUS_ERR;
  return diag_warning_count() > warnings_before ? STATUS_WARN : STATUS_OK;
}

// Prints the output of a failed external tool, indented under its stage.
static void print_tool_output(const char *out) {
  if (!out || !*out)
    return;
  for (const char *line = out; *line;) {
    const char *nl = strchr(line, '\n');
    int len = nl ? (int)(nl - line) : (int)strlen(line);
    fprintf(stderr, "      %.*s\n", len, line);
    line += len + (nl ? 1 : 0);
  }
}

// ---------------------------------------------------------------------------
// In-process stages
// ---------------------------------------------------------------------------

typedef struct {
  const char *path;
  Token *tok;
  PreprocStats stats;
} PreprocessJob;

static void do_preprocess(void *p) {
  PreprocessJob *j = p;
  j->tok = preprocess_file(j->path, &j->stats);
}

typedef struct {
  Token *tok;
  Program *prog;
} ParseJob;

static void do_parse(void *p) {
  ParseJob *j = p;
  preproc_finalize(j->tok);
  j->prog = parse(j->tok);
}

typedef struct {
  Program *prog;
  OptStats stats;
} OptimizeJob;

static void do_optimize(void *p) {
  OptimizeJob *j = p;
  optimize_program(j->prog, &j->stats);
}

typedef struct {
  Program *prog;
  const char *path;
  bool optimize;
  CodegenStats stats;
} CodegenJob;

static void do_codegen(void *p) {
  CodegenJob *j = p;
  FILE *out = fopen(j->path, "w");
  if (!out)
    fatal("cannot write '%s': %s", j->path, strerror(errno));
  codegen(j->prog, out, j->optimize, &j->stats);
  if (fclose(out) != 0)
    fatal("error writing '%s': %s", j->path, strerror(errno));
}

// ---------------------------------------------------------------------------
// Per-file work
// ---------------------------------------------------------------------------

static char *output_name(Build *b, const char *input, const char *ext, int index) {
  Options *o = b->opts;
  bool is_final = (o->stop == STOP_ASSEMBLY && strcmp(ext, ".s") == 0) ||
                  (o->stop == STOP_OBJECT && strcmp(ext, ".o") == 0);
  if (is_final)
    return o->name ? xstrdup(o->name) : format("%s%s", strip_extension(input), ext);
  if (o->keep)
    return format("%s%s", strip_extension(input), ext);
  return format("%s/%d-%s%s", b->tmpdir, index, strip_extension(input), ext);
}

// Assembles `src`; the object is named after the original input file.
static bool assemble(Build *b, const char *src, const char *input, int index) {
  char *obj = output_name(b, input, ".o", index);
  StrVec cmd = assemble_command(src, obj);
  if (b->opts->verbose)
    report_command(join_argv(cmd.data));
  ProcResult r;
  bool ok = run_process(cmd.data, &r);
  add_time(&b->total, r.time);
  char *detail = ok ? format("%s (%s)", file_part(obj), format_size(file_size(obj))) : nullptr;
  report_stage("assemble", ok ? STATUS_OK : STATUS_ERR, r.time, detail);
  if (!ok) {
    print_tool_output(r.output);
    b->tool_errors++;
    return false;
  }
  vec_push(&b->objects, obj);
  return true;
}

static bool write_preprocessed(Build *b, Token *tok) {
  const char *name = b->opts->name;
  FILE *out = name ? fopen(name, "w") : stdout;
  if (!out) {
    fprintf(stderr, "occ: error: cannot write '%s': %s\n", name, strerror(errno));
    b->tool_errors++;
    return false;
  }
  print_tokens(out, tok);
  if (name)
    fclose(out);
  else
    fflush(stdout);
  return true;
}

static bool compile_c_file(Build *b, const char *path, int index) {
  Options *o = b->opts;
  bool optimize = o->opt == OPT_PROD;
  report_file(path);
  preproc_init(&b->pp);

  // 1. Preprocess.
  int w0 = diag_warning_count(), e0 = diag_error_count();
  PreprocessJob pj = {.path = path};
  Duration t;
  bool ok = guarded(do_preprocess, &pj, &t);
  add_time(&b->total, t);
  char *detail = ok ? format("%d %s, %d tokens, %d macro expansions", pj.stats.files,
                             plural(pj.stats.files, "file", "files"), pj.stats.tokens, pj.stats.expansions)
                    : failure_detail(e0);
  report_stage("preprocess", status_of(ok, w0), t, detail);
  diag_flush();
  if (!ok)
    return false;
  if (o->stop == STOP_PREPROCESS)
    return write_preprocessed(b, pj.tok);

  // 2. Compile: parse and check, optimize, generate assembly.
  w0 = diag_warning_count();
  e0 = diag_error_count();
  Duration tp = {}, to = {}, tg = {};
  ParseJob parse_job = {.tok = pj.tok};
  OptimizeJob opt_job = {};
  char *asm_path = output_name(b, path, ".s", index);
  CodegenJob gen_job = {.path = asm_path, .optimize = optimize};

  ok = guarded(do_parse, &parse_job, &tp);
  if (ok && optimize) {
    opt_job.prog = parse_job.prog;
    ok = guarded(do_optimize, &opt_job, &to);
  }
  if (ok) {
    gen_job.prog = parse_job.prog;
    ok = guarded(do_codegen, &gen_job, &tg);
  }

  Duration tc = tp;
  add_time(&tc, to);
  add_time(&tc, tg);
  add_time(&b->total, tc);
  Program *prog = parse_job.prog;
  detail = ok ? format("%d %s, %d %s, %d instructions", prog->functions,
                       plural(prog->functions, "function", "functions"), prog->variables,
                       plural(prog->variables, "global", "globals"), gen_job.stats.instructions)
              : failure_detail(e0);
  report_stage("compile", status_of(ok, w0), tc, detail);
  if (ok) {
    report_substage("parse+check", false, tp, "typed AST");
    if (optimize)
      report_substage("optimize", false, to,
                      format("%d folded, %d simplified, %d dead branches", opt_job.stats.folded,
                             opt_job.stats.simplified, opt_job.stats.branches));
    char *gen_detail = optimize ? format("%d vars in registers, %d peephole removals, %d jump tables",
                                         gen_job.stats.promoted_vars, gen_job.stats.peephole_removed,
                                         gen_job.stats.jump_tables)
                                : xstrdup("stack-machine code");
    report_substage("codegen", true, tg, gen_detail);
  }
  diag_flush();
  if (!ok || o->stop == STOP_ASSEMBLY)
    return ok;

  // 3. Assemble.
  return assemble(b, asm_path, path, index);
}

static bool link_executable(Build *b, const char *output) {
  Options *o = b->opts;
  LinkJob job = {
      .objects = b->objects,
      .libs = o->libs,
      .lib_dirs = o->lib_dirs,
      .output = output,
      .is_static = o->is_static,
  };
  const char *driver;
  StrVec cmd = link_command(&job, &driver);
  if (o->verbose)
    report_command(join_argv(cmd.data));
  ProcResult r;
  bool ok = run_process(cmd.data, &r);
  add_time(&b->total, r.time);
  const char *kind = strcmp(driver, "cc") == 0 ? "linked via cc"
                     : o->is_static           ? "static"
                                              : "PIE, full RELRO, NX stack";
  char *detail = ok ? format("%s (%s, %s)", output, format_size(file_size(output)), kind) : nullptr;
  report_stage("link", ok ? STATUS_OK : STATUS_ERR, r.time, detail);
  if (!ok) {
    print_tool_output(r.output);
    b->tool_errors++;
  }
  return ok;
}

static int run_program(Build *b, const char *path) {
  char *exe = strchr(path, '/') ? xstrdup(path) : format("./%s", path);
  StrVec argv = {};
  vec_push(&argv, exe);
  for (size_t i = 0; i < b->opts->run_args.len; i++)
    vec_push(&argv, b->opts->run_args.data[i]);
  vec_push(&argv, nullptr);

  fflush(stdout);
  fflush(stderr);
  pid_t pid;
  if (posix_spawn(&pid, exe, nullptr, nullptr, argv.data, environ) != 0) {
    fprintf(stderr, "occ: error: cannot run '%s'\n", exe);
    return 1;
  }
  int status;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      return 1;
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "occ: '%s' was terminated by signal %d (%s)\n", exe, WTERMSIG(status),
            strsignal(WTERMSIG(status)));
    return 128 + WTERMSIG(status);
  }
  return WEXITSTATUS(status);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static void setup_preprocessor(Build *b) {
  Options *o = b->opts;
  b->pp = (PreprocOptions){
      .quote_dirs = o->quote_dirs,
      .user_dirs = o->include_dirs,
      .defines = o->defines,
      .undefines = o->undefines,
      .optimize = o->opt == OPT_PROD,
  };
  char *own = find_occ_include_dir();
  if (own)
    vec_push(&b->pp.system_dirs, own);
  else
    fprintf(stderr, "occ: warning: cannot find occ's own include directory; <stddef.h> and friends "
                    "will be missing\n");
  StrVec sys = system_include_dirs();
  for (size_t i = 0; i < sys.len; i++)
    vec_push(&b->pp.system_dirs, sys.data[i]);
}

int run_pipeline(Options *opts) {
  diag_configure(opts->color, opts->werror, opts->no_warnings);
  report_init(!opts->quiet, opts->color);

  Build b = {.opts = opts, .tmpdir = make_temp_dir()};
  setup_preprocessor(&b);
  report_header(opts->opt == OPT_PROD ? "prod" : "none", "x86-64 Linux");

  bool ok = true;
  for (size_t i = 0; i < opts->inputs.len; i++) {
    char *in = opts->inputs.data[i];
    if (access(in, R_OK) != 0) {
      fprintf(stderr, "occ: error: cannot read '%s': %s\n", in, strerror(errno));
      b.tool_errors++;
      ok = false;
      continue;
    }
    if (ends_with(in, ".c") || strcmp(in, "-") == 0) {
      ok &= compile_c_file(&b, in, (int)i);
    } else if (ends_with(in, ".s") || ends_with(in, ".S")) {
      if (opts->stop == STOP_EXECUTABLE || opts->stop == STOP_OBJECT) {
        report_file(in);
        ok &= assemble(&b, in, in, (int)i);
      }
    } else {
      vec_push(&b.objects, in); // object files and archives go to the linker
    }
  }

  char *exe = nullptr;
  if (ok && opts->stop == STOP_EXECUTABLE) {
    exe = opts->name ? opts->name : strip_extension(opts->inputs.data[0]);
    ok = link_executable(&b, exe);
  }

  remove_temp_dir(b.tmpdir);
  diag_flush();

  int errors = diag_error_count() + b.tool_errors;
  if (opts->stop != STOP_PREPROCESS || !ok) {
    const char *what = exe                             ? format("built %s", exe)
                       : opts->stop == STOP_ASSEMBLY   ? "generated assembly"
                       : opts->stop == STOP_OBJECT     ? "generated object files"
                                                       : "preprocessed";
    report_summary(ok, what, b.total, errors, diag_warning_count());
  }
  if (!ok)
    return 1;
  if (opts->run)
    return run_program(&b, exe);
  return 0;
}

// cli.c - command-line parsing.
#include "driver/cli.h"

#include <unistd.h>

#include "support/diag.h"

static void usage(FILE *out) {
  fputs("Usage: occ [options] <file>...\n"
        "\n"
        "occ compiles C23 source files into x86-64 Linux executables.\n"
        "Inputs may be .c (compiled), .s (assembled), .o and .a files (linked).\n"
        "\n"
        "Main options:\n"
        "  -o <level>        Optimization level: 'none' (default) or 'prod'\n"
        "                    ('prod' is production-ready code, roughly GCC -O1/-O2)\n"
        "  -n <name>         Name of the output file (default: the source file's\n"
        "                    name without its extension)\n"
        "\n"
        "Pipeline control:\n"
        "  -E                Stop after preprocessing (prints to stdout unless -n)\n"
        "  -S                Stop after compiling; write assembly (<name>.s)\n"
        "  -c                Stop after assembling; write an object file (<name>.o)\n"
        "  --run [-- args]   Run the program after building it\n"
        "  --keep            Keep intermediate files (.s, .o) next to the output\n"
        "\n"
        "Preprocessor:\n"
        "  -I <dir>          Add a directory to the #include <...> search path\n"
        "  -iquote <dir>     Add a directory to the #include \"...\" search path\n"
        "  -D <name>[=val]   Define a macro\n"
        "  -U <name>         Undefine a macro\n"
        "\n"
        "Linker:\n"
        "  -l <lib>          Link with a library (libm is linked automatically)\n"
        "  -L <dir>          Add a library search directory\n"
        "  --static          Produce a statically linked executable\n"
        "  -rdynamic         Export all symbols (for plugins loaded with dlopen)\n"
        "\n"
        "Diagnostics:\n"
        "  -w                Suppress all warnings\n"
        "  -Werror           Treat warnings as errors\n"
        "  -W<name>          Enable a warning (all are on by default)\n"
        "  -Wno-<name>       Disable a warning\n"
        "  --warnings        List all warnings\n"
        "  -q                Quiet: print diagnostics only, no pipeline status\n"
        "  -v                Verbose: also show the external commands that are run\n"
        "  --color, --no-color  Force or disable colored output\n"
        "\n"
        "  -h, --help        Show this help\n"
        "  --version         Show version information\n",
        out);
}

[[noreturn]] static void die(const char *fmt, const char *arg) {
  fprintf(stderr, "occ: error: ");
  fprintf(stderr, fmt, arg);
  fprintf(stderr, "\nTry 'occ --help' for more information.\n");
  exit(2);
}

// Returns the value of an option given as "-Xvalue" or "-X value".
static char *take_value(int argc, char **argv, int *i, const char *opt) {
  size_t n = strlen(opt);
  if (argv[*i][n])
    return argv[*i] + n + (argv[*i][n] == '=' ? 1 : 0);
  if (*i + 1 >= argc)
    die("missing argument to '%s'", opt);
  return argv[++*i];
}

static OptLevel parse_opt_level(const char *v) {
  if (strcmp(v, "none") == 0 || strcmp(v, "0") == 0)
    return OPT_NONE;
  if (strcmp(v, "prod") == 0 || strcmp(v, "1") == 0 || strcmp(v, "2") == 0 || strcmp(v, "3") == 0)
    return OPT_PROD;
  die("unknown optimization level '%s' (expected 'none' or 'prod')", v);
}

static bool want_color(void) {
  const char *term = getenv("TERM");
  return isatty(STDERR_FILENO) && !getenv("NO_COLOR") && !(term && strcmp(term, "dumb") == 0);
}

Options parse_args(int argc, char **argv) {
  Options o = {.stop = STOP_EXECUTABLE, .color = want_color()};

  for (int i = 1; i < argc; i++) {
    char *a = argv[i];

    if (strcmp(a, "--") == 0) {
      for (i++; i < argc; i++)
        vec_push(&o.run_args, argv[i]);
      break;
    }
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      exit(0);
    }
    if (strcmp(a, "--version") == 0) {
      printf("occ " OCC_VERSION "\n"
             "Target: x86_64-linux-gnu, language: C23\n");
      exit(0);
    }
    if (strcmp(a, "--warnings") == 0) {
      puts("Warnings (all enabled by default; disable with -Wno-<name>):");
      diag_print_warning_list(stdout);
      exit(0);
    }

    if (starts_with(a, "-o")) {
      o.opt = parse_opt_level(take_value(argc, argv, &i, "-o"));
    } else if (starts_with(a, "--opt")) {
      o.opt = parse_opt_level(take_value(argc, argv, &i, "--opt"));
    } else if (a[0] == '-' && a[1] == 'O') { // GCC-style aliases: -O0, -O2
      o.opt = parse_opt_level(a[2] ? a + 2 : "2");
    } else if (starts_with(a, "-n")) {
      o.name = take_value(argc, argv, &i, "-n");
    } else if (strcmp(a, "-E") == 0) {
      o.stop = STOP_PREPROCESS;
    } else if (strcmp(a, "-S") == 0) {
      o.stop = STOP_ASSEMBLY;
    } else if (strcmp(a, "-c") == 0) {
      o.stop = STOP_OBJECT;
    } else if (strcmp(a, "--run") == 0) {
      o.run = true;
    } else if (strcmp(a, "--keep") == 0) {
      o.keep = true;
    } else if (strcmp(a, "--static") == 0) {
      o.is_static = true;
    } else if (strcmp(a, "-rdynamic") == 0) {
      o.export_dynamic = true;
    } else if (starts_with(a, "-iquote")) {
      vec_push(&o.quote_dirs, take_value(argc, argv, &i, "-iquote"));
    } else if (starts_with(a, "-I")) {
      vec_push(&o.include_dirs, take_value(argc, argv, &i, "-I"));
    } else if (starts_with(a, "-D")) {
      vec_push(&o.defines, take_value(argc, argv, &i, "-D"));
    } else if (starts_with(a, "-U")) {
      vec_push(&o.undefines, take_value(argc, argv, &i, "-U"));
    } else if (starts_with(a, "-l")) {
      vec_push(&o.libs, take_value(argc, argv, &i, "-l"));
    } else if (starts_with(a, "-L")) {
      vec_push(&o.lib_dirs, take_value(argc, argv, &i, "-L"));
    } else if (strcmp(a, "-w") == 0) {
      o.no_warnings = true;
    } else if (strcmp(a, "-Werror") == 0) {
      o.werror = true;
    } else if (starts_with(a, "-Wno-")) {
      if (!diag_set_warning(a + 5, false))
        die("unknown warning option '%s' (see occ --warnings)", a);
    } else if (starts_with(a, "-W") && a[2]) {
      if (!diag_set_warning(a + 2, true))
        die("unknown warning option '%s' (see occ --warnings)", a);
    } else if (strcmp(a, "-q") == 0) {
      o.quiet = true;
    } else if (strcmp(a, "-v") == 0) {
      o.verbose = true;
    } else if (strcmp(a, "--color") == 0) {
      o.color = true;
    } else if (strcmp(a, "--no-color") == 0) {
      o.color = false;
    } else if (strcmp(a, "-std=c23") == 0 || strcmp(a, "-std=c2x") == 0 || strcmp(a, "-g") == 0) {
      // Accepted for compatibility: C23 is the only dialect; no debug info.
    } else if (a[0] == '-' && a[1]) {
      die("unknown option '%s'", a);
    } else {
      vec_push(&o.inputs, a);
    }
  }

  if (o.inputs.len == 0)
    die("%s", "no input files");
  if (o.name && o.inputs.len > 1 && (o.stop == STOP_ASSEMBLY || o.stop == STOP_OBJECT))
    die("%s", "-n cannot be used with -S or -c and several input files");
  if (o.run && o.stop != STOP_EXECUTABLE)
    die("%s", "--run requires building an executable");
  return o;
}

// toolchain.c - running the assembler and linker.
#include "driver/toolchain.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "support/strbuf.h"

extern char **environ;

// ---------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------

static double tv_ms(struct timeval tv) { return (double)tv.tv_sec * 1e3 + (double)tv.tv_usec / 1e3; }

bool run_process(char *const *argv, ProcResult *res) {
  *res = (ProcResult){.exit_code = -1};
  int fds[2];
  if (pipe(fds) != 0)
    return false;

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&fa, fds[1]);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  pid_t pid;
  int err = posix_spawnp(&pid, argv[0], &fa, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (err != 0) {
    close(fds[0]);
    res->output = format("cannot execute '%s': %s\n", argv[0], strerror(err));
    return false;
  }

  StrBuf out = {};
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
    if (n > 0)
      sb_append(&out, buf, (size_t)n);
  close(fds[0]);

  int status;
  struct rusage ru;
  while (wait4(pid, &status, 0, &ru) < 0)
    if (errno != EINTR)
      return false;
  clock_gettime(CLOCK_MONOTONIC, &t1);

  res->time.cpu_ms = tv_ms(ru.ru_utime) + tv_ms(ru.ru_stime);
  res->time.wall_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
  res->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  res->output = out.data ? out.data : xstrdup("");
  return res->exit_code == 0;
}

char *join_argv(char *const *argv) {
  StrBuf sb = {};
  for (int i = 0; argv[i]; i++) {
    if (i)
      sb_putc(&sb, ' ');
    sb_puts(&sb, argv[i]);
  }
  return sb.data;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static bool exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

long file_size(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

char *make_temp_dir(void) {
  const char *tmp = getenv("TMPDIR");
  char *templ = format("%s/occ-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  if (!mkdtemp(templ)) {
    fprintf(stderr, "occ: error: cannot create a temporary directory: %s\n", strerror(errno));
    exit(1);
  }
  return templ;
}

void remove_temp_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)))
    if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
      unlink(format("%s/%s", dir, e->d_name));
  closedir(d);
  rmdir(dir);
}

static char *exe_dir(void) {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0)
    return nullptr;
  buf[n] = '\0';
  char *slash = strrchr(buf, '/');
  if (slash)
    *slash = '\0';
  return xstrdup(buf);
}

char *find_occ_include_dir(void) {
  char *dir = exe_dir();
  if (dir) {
    // Build tree (./occ next to include/) and stage-2 build (build/stage2/occ).
    const char *candidates[] = {"%s/include", "%s/../lib/occ/include", "%s/../../include"};
    for (size_t i = 0; i < ARRAY_LEN(candidates); i++) {
      char *path = format(candidates[i], dir);
      if (exists(format("%s/stddef.h", path)))
        return path;
    }
  }
#ifdef OCC_INSTALL_INCLUDE_DIR
  if (exists(OCC_INSTALL_INCLUDE_DIR "/stddef.h"))
    return xstrdup(OCC_INSTALL_INCLUDE_DIR);
#endif
  return nullptr;
}

StrVec system_include_dirs(void) {
  static const char *dirs[] = {"/usr/local/include", "/usr/include/x86_64-linux-gnu", "/usr/include"};
  StrVec v = {};
  for (size_t i = 0; i < ARRAY_LEN(dirs); i++)
    if (exists(dirs[i]))
      vec_push(&v, (char *)dirs[i]);
  return v;
}

// ---------------------------------------------------------------------------
// Assembler and linker
// ---------------------------------------------------------------------------

static const char *tool(const char *env, const char *fallback) {
  const char *v = getenv(env);
  return v && *v ? v : fallback;
}

StrVec assemble_command(const char *src, const char *obj) {
  StrVec v = {};
  vec_push(&v, (char *)tool("OCC_AS", "as"));
  vec_push(&v, "--64");
  vec_push(&v, "-o");
  vec_push(&v, (char *)obj);
  vec_push(&v, (char *)src);
  vec_push(&v, nullptr);
  return v;
}

static const char *const lib_dirs[] = {
    "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib64", "/lib64", "/usr/lib", "/lib",
};

static char *find_in_lib_dirs(const char *name) {
  for (size_t i = 0; i < ARRAY_LEN(lib_dirs); i++) {
    char *path = format("%s/%s", lib_dirs[i], name);
    if (exists(path))
      return path;
  }
  return nullptr;
}

// GCC's runtime directory (crtbegin.o, libgcc.a): the newest version found.
static char *find_gcc_lib_dir(void) {
  static const char *patterns[] = {
      "/usr/lib/gcc/x86_64-linux-gnu/*/crtbeginS.o",   "/usr/lib/gcc/x86_64-pc-linux-gnu/*/crtbeginS.o",
      "/usr/lib/gcc/x86_64-redhat-linux/*/crtbeginS.o", "/usr/lib64/gcc/x86_64-suse-linux/*/crtbeginS.o",
      "/usr/lib/gcc/x86_64-alpine-linux-musl/*/crtbeginS.o",
  };
  char *best = nullptr;
  long best_ver = -1;
  for (size_t i = 0; i < ARRAY_LEN(patterns); i++) {
    glob_t g;
    if (glob(patterns[i], 0, nullptr, &g) != 0)
      continue;
    for (size_t k = 0; k < g.gl_pathc; k++) {
      char *path = xstrdup(g.gl_pathv[k]);
      *strrchr(path, '/') = '\0';
      long ver = strtol(strrchr(path, '/') + 1, nullptr, 10);
      if (ver > best_ver) {
        best_ver = ver;
        best = path;
      }
    }
    globfree(&g);
  }
  return best;
}

static void push_all(StrVec *v, const StrVec *items, const char *prefix) {
  for (size_t i = 0; i < items->len; i++)
    vec_push(v, prefix ? format("%s%s", prefix, items->data[i]) : items->data[i]);
}

static StrVec cc_fallback(const LinkJob *job) {
  StrVec v = {};
  vec_push(&v, (char *)tool("OCC_CC", "cc"));
  vec_push(&v, "-o");
  vec_push(&v, (char *)job->output);
  if (job->is_static)
    vec_push(&v, "-static");
  push_all(&v, &job->lib_dirs, "-L");
  push_all(&v, &job->objects, nullptr);
  push_all(&v, &job->libs, "-l");
  vec_push(&v, "-lm");
  vec_push(&v, nullptr);
  return v;
}

StrVec link_command(const LinkJob *job, const char **driver_name) {
  char *crt1 = find_in_lib_dirs(job->is_static ? "crt1.o" : "Scrt1.o");
  char *crti = find_in_lib_dirs("crti.o");
  char *crtn = find_in_lib_dirs("crtn.o");
  char *gcc_dir = find_gcc_lib_dir();
  const char *dynamic_linker = exists("/lib64/ld-linux-x86-64.so.2") ? "/lib64/ld-linux-x86-64.so.2"
                                                                     : "/lib/ld-linux-x86-64.so.2";
  if (!crt1 || !crti || !crtn || (job->is_static && !gcc_dir) || getenv("OCC_LINK_WITH_CC")) {
    *driver_name = "cc";
    return cc_fallback(job);
  }
  *driver_name = "ld";

  StrVec v = {};
  vec_push(&v, (char *)tool("OCC_LD", "ld"));
  vec_push(&v, "-m");
  vec_push(&v, "elf_x86_64");
  vec_push(&v, "--eh-frame-hdr");
  if (job->is_static) {
    vec_push(&v, "-static");
  } else {
    // Hardened defaults: position independent (ASLR), full RELRO.
    vec_push(&v, "-pie");
    vec_push(&v, "-dynamic-linker");
    vec_push(&v, (char *)dynamic_linker);
    vec_push(&v, "-z");
    vec_push(&v, "relro");
    vec_push(&v, "-z");
    vec_push(&v, "now");
  }
  vec_push(&v, "-z");
  vec_push(&v, "noexecstack");
  vec_push(&v, "-o");
  vec_push(&v, (char *)job->output);

  vec_push(&v, crt1);
  vec_push(&v, crti);
  if (gcc_dir)
    vec_push(&v, format("%s/%s", gcc_dir, job->is_static ? "crtbeginT.o" : "crtbeginS.o"));

  push_all(&v, &job->lib_dirs, "-L");
  if (gcc_dir)
    vec_push(&v, format("-L%s", gcc_dir));
  for (size_t i = 0; i < ARRAY_LEN(lib_dirs); i++)
    if (exists(lib_dirs[i]))
      vec_push(&v, format("-L%s", lib_dirs[i]));

  push_all(&v, &job->objects, nullptr);
  push_all(&v, &job->libs, "-l");

  if (job->is_static) {
    vec_push(&v, "--start-group");
    vec_push(&v, "-lm");
    vec_push(&v, "-lc");
    vec_push(&v, "-lgcc");
    vec_push(&v, "-lgcc_eh");
    vec_push(&v, "--end-group");
  } else {
    // libm is linked for convenience but only recorded if actually used.
    vec_push(&v, "--as-needed");
    vec_push(&v, "-lm");
    vec_push(&v, "--no-as-needed");
    vec_push(&v, "-lc");
  }

  if (gcc_dir)
    vec_push(&v, format("%s/%s", gcc_dir, job->is_static ? "crtend.o" : "crtendS.o"));
  vec_push(&v, crtn);
  vec_push(&v, nullptr);
  return v;
}

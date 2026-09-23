<div align="center">

# occ — the Opus C Compiler

**A self-hosting C23 compiler for x86-64 Linux, written in C23.**

Preprocessor · parser · optimizer · code generator — with a pipeline you can watch.

[![CI](https://github.com/foxik38/opus-c-compiler/actions/workflows/ci.yml/badge.svg)](https://github.com/foxik38/opus-c-compiler/actions/workflows/ci.yml)
![C23](https://img.shields.io/badge/language-C23-blue)
![x86-64 Linux](https://img.shields.io/badge/target-x86--64%20Linux-informational)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

</div>

```text
$ occ -o prod life.c
occ 1.0.0 · x86-64 Linux · C23 · opt=prod
  life.c
    preprocess  OK     16.31 ms cpu   17.33 ms wall  59 files, 6263 tokens, 1378 macro expansions
    compile     OK      2.68 ms cpu    2.71 ms wall  4 functions, 3 globals, 231 instructions
      ├ parse+check    2.07 ms cpu                  typed AST
      ├ optimize       0.07 ms cpu                  14 folded, 26 simplified, 0 dead branches, 4 hoisted from loops
      └ codegen        0.54 ms cpu                  17 vars in registers, 4 peephole removals, 0 jump tables
    assemble    OK      3.35 ms cpu    3.42 ms wall  life.o (2.5 KiB)
    link        OK     19.56 ms cpu   19.79 ms wall  life (15.9 KiB, PIE, full RELRO, NX stack)
  ✓ built life in 43.26 ms (cpu 41.88 ms) · 0 errors, 0 warnings
```

## Highlights

- **Complete pipeline** — its own preprocessor, parser, type checker, optimizer
  and x86-64 code generator; GNU `as` and `ld` for the last mile, driven
  directly (occ finds the C runtime objects itself).
- **Watchable** — every stage reports `OK`/`ERR`, CPU and wall time, and what
  it produced.
- **Helpful diagnostics** — GCC/Clang-style errors with source excerpts, and
  27 named warnings aimed at undefined behavior and deprecated features.
- **Two optimization levels** — `-o none` for obviously-correct code,
  `-o prod` for register allocation, addressing modes, constant folding,
  strength reduction, loop-invariant code motion, jump tables and a
  peephole pass. On loop-heavy code it lands near `gcc -O2`.
- **C23** — `constexpr`, `auto`, `typeof`, `nullptr`, `bool` keywords,
  `#embed`, attributes, `<stdckdint.h>`, typed enums, digit separators and more.
- **Real headers** — compiles against the system's glibc headers unchanged.
- **Self-hosting** — occ compiles itself and reaches a bootstrap fixpoint.
- **Tested four ways** — self-checking tests at both optimization levels,
  diagnostic tests, the self-host fixpoint, and differential fuzzing
  against GCC.

## Quick start

Requirements: a C23 (or C2x) compiler to bootstrap (tested with GCC 13 and Clang 18),
GNU binutils and the glibc development files — i.e. `build-essential` on
Debian/Ubuntu, `base-devel` on Arch.

```sh
make                        # builds ./occ
make test                   # every test at -o none and -o prod
sudo make install           # /usr/local/bin/occ (+ its headers in /usr/local/lib/occ)

occ examples/hello.c --run  # build and run
occ -o prod -n life examples/life.c && ./life 30
```

## Usage

```text
occ [options] <file>...
```

Inputs may be `.c` (compiled), `.s` (assembled), `.o` and `.a` files (linked).

> [!NOTE]
> In occ, **`-o` selects the optimization level** and **`-n` names the
> output** — not the other way round as in GCC.

| Option | Meaning |
|---|---|
| `-o none` \| `-o prod` | Optimization level. `none` is the default; `prod` is production-ready code (≈ GCC `-O1`/`-O2`). `-O0`…`-O3` are accepted as aliases. |
| `-n <name>` | Output file name. Default: the first source file's name without its extension. |
| `-E` / `-S` / `-c` | Stop after preprocessing / compiling (`.s`) / assembling (`.o`). |
| `--run [-- args]` | Run the program after building it. |
| `--keep` | Keep the intermediate `.s` and `.o` files. |
| `-I <dir>`, `-iquote <dir>` | Add `<...>` / `"..."` include directories. |
| `-D <name>[=val]`, `-U <name>` | Define / undefine a macro. |
| `-l <lib>`, `-L <dir>`, `--static` | Linker options (`libm` is linked automatically, only if used). |
| `-w`, `-Werror`, `-W<name>`, `-Wno-<name>` | Control warnings; `--warnings` lists them all. |
| `-q`, `-v` | Quiet (diagnostics only) / verbose (also show `as` and `ld` commands). |
| `--color`, `--no-color` | Force colors on or off (default: when stderr is a terminal, honoring `NO_COLOR`). |

Environment: `OCC_AS`, `OCC_LD` and `OCC_CC` override the tools occ runs;
`OCC_LINK_WITH_CC=1` links through `cc` instead of calling `ld` directly.

## Diagnostics

When something goes wrong, the failing stage turns into `ERR` and the
diagnostics follow, each with its location, a source excerpt and the name of
the warning that produced it:

```text
$ occ bad.c
occ 1.0.0 · x86-64 Linux · C23 · opt=none
  bad.c
    preprocess  OK      8.04 ms cpu    8.11 ms wall  27 files, 2124 tokens, 588 macro expansions
    compile     ERR     0.70 ms cpu    0.70 ms wall  1 error
bad.c:6:13: warning: using the result of an assignment as a condition without parentheses [-Wparentheses]
     6 |   if (count = 3)
       |             ^
bad.c:7:20: warning: format specifies type 'char *' but the argument has type 'unsigned int' [-Wformat]
     7 |     printf("%s\n", n);
       |                    ^
bad.c:8:21: warning: comparison of integers of different signs: 'int' and 'unsigned int' [-Wsign-compare]
     8 |   for (int i = 0; i < n; i++)
       |                     ^
bad.c:10:10: error: use of undeclared identifier 'undefined_thing'
    10 |   return undefined_thing;
       |          ^~~~~~~~~~~~~~~
  ✗ build failed · 1 error, 3 warnings
```

All warnings are on by default. Those marked **UB** flag code whose behavior
the C standard leaves undefined.

<details>
<summary><b>All 27 warnings</b> (<code>occ --warnings</code>)</summary>

| Warning | Detects |
|---|---|
| `unused-variable` | local variable is never used |
| `unused-function` | static function is never used |
| `unused-value` | expression result is computed and discarded |
| `unused-result` | result of a `[[nodiscard]]` function is ignored |
| `uninitialized` | variable is read but never written — **UB** |
| `parentheses` | assignment used as a truth value |
| `empty-body` | empty body of `if`/`while`/`for` (stray semicolon) |
| `div-by-zero` | integer division by constant zero — **UB** |
| `shift-count` | shift count negative or ≥ width of type — **UB** |
| `overflow` | constant overflows or changes value on conversion |
| `array-bounds` | constant index outside of array bounds — **UB** |
| `null-dereference` | dereference of a null pointer constant — **UB** |
| `return-local-addr` | returning the address of a local variable — **UB** |
| `return-type` | control reaches end of non-void function — **UB** |
| `format` | `printf`/`scanf` format does not match arguments — **UB** |
| `sign-compare` | comparison between signed and unsigned integers |
| `int-conversion` | implicit conversion between pointer and integer |
| `incompatible-pointer-types` | implicit conversion between incompatible pointer types |
| `discarded-qualifiers` | implicit conversion drops `const`/`volatile` |
| `string-compare` | comparing against a string literal with `==` or `!=` |
| `sizeof-array-argument` | `sizeof` applied to an array parameter |
| `deprecated` | use of deprecated, obsolescent or unsafe features |
| `multichar` | multi-character character constant |
| `main` | suspicious declaration of `main` |
| `macro-redefined` | macro redefined with a different body |
| `cpp` | `#warning` directive |
| `unsupported` | feature accepted but only approximated by occ |

Removed features are hard errors: implicit `int` and implicit function
declarations (removed in C99), `gets()` (removed in C11) and K&R-style
function definitions (removed in C23).

</details>

## Optimization

| | `-o none` | `-o prod` |
|---|---|---|
| Code model | accumulator + stack machine | same skeleton, costs removed |
| Variables | on the stack | scalars in `%rbx`, `%r12`–`%r15`; `double`/`float` in `%xmm12`–`%xmm15` |
| Temporaries | `push`/`pop` | scratch registers `%r8`–`%r11`, `%xmm8`–`%xmm11` |
| Memory access | address in `%rax`, then load | `disp(base, index, scale)` operands, read-modify-write, immediate stores |
| Control flow | test at the loop top | rotated loops, direct `cmp`+`jcc`, jump tables for dense `switch` |
| AST passes | — | constant folding, algebraic identities, strength reduction, dead branches, loop-invariant code motion |
| Cleanup | — | peephole pass |

### Benchmarks

Best of 3 runs on the benchmarks in [`tests/bench`](tests/bench), measured
with `make bench` (checksums of all builds agree). Lower is better.

| Benchmark | occ `-o none` | occ `-o prod` | gcc `-O0` | gcc `-O2` |
|---|---:|---:|---:|---:|
| `fib` (recursion) | 0.099 s | 0.072 s | 0.087 s | 0.030 s |
| `hashmap` (memory-bound) | 0.524 s | 0.478 s | 0.484 s | 0.481 s |
| `mandelbrot` (floating point) | 0.487 s | 0.237 s | 0.357 s | 0.205 s |
| `matmul` (nested loops) | 0.212 s | 0.050 s | 0.120 s | 0.023 s |
| `sieve` (array scans) | 2.273 s | 0.824 s | 2.068 s | 1.102 s |
| `sort` (quicksort) | 1.222 s | 0.495 s | 0.709 s | 0.463 s |

`-o prod` is up to 4.2× faster than `-o none` and beats `gcc -O0` on every
benchmark. It is within 20% of `gcc -O2` on four of six; the remaining gap
is call-heavy code (`fib`: no inlining, callee-saved registers always saved)
and vectorizable loops (`matmul`). Timings are from a shared container, so
expect noise of about ±10% — `sieve` beating `gcc -O2` is within it.

## C23 support

<details open>
<summary>What's in</summary>

- `constexpr` objects, `auto` type inference, `typeof` / `typeof_unqual`
- `nullptr` / `nullptr_t`, `bool` / `true` / `false` as keywords
- Enumerations with a fixed underlying type (`enum e : uint8_t`)
- Attributes: `[[nodiscard]]`, `[[deprecated]]`, `[[fallthrough]]`,
  `[[maybe_unused]]`, `[[noreturn]]`, `[[unsequenced]]`, `[[reproducible]]`
- `#embed`, `#elifdef` / `#elifndef`, `#warning`, `__has_include`,
  `__has_embed`, `__has_c_attribute`, `__VA_OPT__`
- `static_assert` without a message, empty initializers `= {}`
- Binary literals and digit separators (`0b1010'0101`)
- Labels before declarations and at the end of blocks
- Unnamed parameters in definitions, `f()` meaning `f(void)`
- `<stdckdint.h>` checked arithmetic, `unreachable()`, `u8` character constants
- Everything from C99/C11 you'd expect: designated initializers, compound
  literals, flexible array members, anonymous structs/unions, `_Generic`,
  `_Alignas`/`_Alignof`, `_Thread_local`, variadic functions, bit-fields,
  `setjmp`/`longjmp`, struct passing and returning by value (SysV ABI)

</details>

<details>
<summary>What's not (yet)</summary>

- Variable length arrays — rejected with an error (optional since C11)
- `_BitInt(N)`, `_Complex`, decimal floating point
- `long double` is compiled as `double`; `_Atomic` is accepted without atomic
  semantics (both reported by `-Wunsupported`)
- GNU statement expressions and extended `asm` (basic `asm("...")` works)
- Targets other than x86-64 Linux with glibc — by design

</details>

A tour of the features lives in [`examples/c23_tour.c`](examples/c23_tour.c).

## Architecture

```text
 source.c ─▶ preprocess ─▶ parse+check ─▶ optimize ─▶ codegen ─▶ as ─▶ ld ─▶ executable
            src/preproc    src/parse      src/opt    src/codegen  (GNU binutils)
             tokens         typed AST      AST        x86-64 asm
```

```text
src/
├── main.c
├── support/   common utilities: vectors, strings, hash map, source files, diagnostics, timers
├── preproc/   lexer, macro expansion (Prosser hidesets), directives, #if evaluation
├── parse/     declarations, expressions, statements, initializers, types, scopes,
│              constant evaluation, and the static checker (check.c) behind most warnings
├── opt/       -o prod AST passes: folding and simplification (fold.c), LICM (licm.c)
├── codegen/   expressions, statements, calls and the SysV ABI, data sections,
│              register allocation, peephole pass
└── driver/    command line, pipeline and stage reporting, external tools
include/       freestanding headers occ provides (stddef.h, stdarg.h, stdckdint.h, ...)
examples/      small programs to try
tests/
├── cases/     self-checking runtime tests
├── diag/      expected warnings and errors
├── bench/     benchmarks and their runner
└── fuzz/      differential fuzzer
```

The parser produces a fully typed AST in which every implicit conversion is
an explicit cast, so the code generator never has to rediscover C's
conversion rules. The reasoning behind this and every other design decision
is written up in [**THINKPROC.md**](THINKPROC.md).

## Testing

```sh
make test                   # runtime + diagnostic tests, at -o none and -o prod
tests/run.sh --reference    # validate the runtime tests themselves with GCC
make selfhost               # occ builds occ; stage 1 and 2 must emit identical assembly
make fuzz FUZZ_COUNT=1000   # random UB-free programs, occ vs GCC
make bench                  # the table above
```

CI runs all of these on every push.

## License

[MIT](LICENSE)

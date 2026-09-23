<div align="center">

<img src="docs/assets/logo.svg" alt="occ — the Opus C Compiler" width="520">

**A self-hosting C23 compiler for x86-64 Linux, written in C23.**<br>
Its own preprocessor, parser, optimizer and code generator — with a build you can watch.

[![CI](https://github.com/foxik38/opus-c-compiler/actions/workflows/ci.yml/badge.svg)](https://github.com/foxik38/opus-c-compiler/actions/workflows/ci.yml)
![C23](https://img.shields.io/badge/language-C23-5fafff)
![x86-64 Linux](https://img.shields.io/badge/target-x86--64%20Linux-5fd7d7)
![License: MIT](https://img.shields.io/badge/license-MIT-a6e3a1)
[![Built with Claude](https://img.shields.io/badge/built%20with-Claude-D97757?logo=claude&logoColor=white)](THINKPROC.md)

**English** · [Čeština](README.cs.md) · [Magyar](README.hu.md) · [עברית](README.he.md)

<img src="docs/assets/demo.svg" alt="occ building a program: each pipeline stage with its status, CPU time and output" width="860">

</div>

## Contents

[Highlights](#highlights) · [Quick start](#quick-start) · [Usage](#usage) · [Diagnostics](#diagnostics) ·
[How it works](#how-it-works) · [Performance](#performance) · [Tested on real code](#tested-on-real-code) ·
[C23 support](#c23-support) · [Testing](#testing) · [Built with Claude](#built-with-claude)

## Highlights

- 🧩 **The whole pipeline** — preprocessor, parser and type checker, optimizer and x86-64
  code generator are occ's own; GNU `as` and `ld` do the last mile, driven directly.
- 📺 **A build you can watch** — every stage reports `OK`/`ERR`, CPU and wall time and what it
  produced. On a terminal, stages animate while they run and the summary shows where the time went.
- 🩺 **Diagnostics that help** — GCC-style messages with source excerpts, a `help:` hint for the
  common mistakes, and 27 named warnings aimed at undefined behavior and deprecated features.
- 🚀 **Two optimization levels** — `-o none` for obviously correct code, `-o prod` for register
  allocation, addressing modes, constant folding, strength reduction, loop-invariant code motion,
  jump tables and a peephole pass. On loop-heavy code it gets close to `gcc -O2`.
- ✨ **C23** — `constexpr`, `auto`, `typeof`, `nullptr`, `bool`, `#embed`, attributes,
  `<stdckdint.h>`, typed enums, digit separators and more.
- 🧪 **Proven on real programs** — SQLite, Lua, zlib, Duktape and friends build with occ and pass
  their own tests; occ compiles itself to a bootstrap fixpoint.

## Quick start

You need a C23 (or C2x) compiler to bootstrap (tested with GCC 13 and Clang 18), GNU binutils and
the glibc development files — `build-essential` on Debian/Ubuntu, `base-devel` on Arch.

```sh
make                          # builds ./occ
make test                     # every test at -o none and -o prod
sudo make install             # /usr/local/bin/occ (+ its headers in /usr/local/lib/occ)

occ examples/hello.c --run    # build and run
occ -o prod examples/life.c && ./life 30
```

## Usage

```text
occ [options] <file>...
```

Inputs may be `.c` (compiled), `.s` (assembled), `.o` and `.a` files (linked).

> [!NOTE]
> In occ, **`-o` selects the optimization level** and **`-n` names the output** — not the other
> way round as in GCC.

| Option | Meaning |
|---|---|
| `-o none` \| `-o prod` | Optimization level. `none` is the default; `prod` is production-ready code (≈ GCC `-O1`/`-O2`). `-O0`…`-O3` are accepted as aliases. |
| `-n <name>` | Output file name. Default: the first source file's name without its extension. |
| `-E` / `-S` / `-c` | Stop after preprocessing / compiling (`.s`) / assembling (`.o`). |
| `--run [-- args]` | Run the program after building it. |
| `--keep` | Keep the intermediate `.s` and `.o` files. |
| `-I <dir>`, `-iquote <dir>` | Add `<...>` / `"..."` include directories. |
| `-D <name>[=val]`, `-U <name>` | Define / undefine a macro. |
| `-l <lib>`, `-L <dir>` | Link with a library / add a library directory (`libm` is linked automatically, only if used). |
| `--static`, `-rdynamic` | Link statically / export all symbols for plugins loaded with `dlopen()`. |
| `-w`, `-Werror`, `-W<name>`, `-Wno-<name>` | Control warnings; `--warnings` lists them all. |
| `-q`, `-v` | Quiet (diagnostics only) / verbose (also show the `as` and `ld` commands). |
| `--color`, `--no-color` | Force colors on or off (default: when stderr is a terminal, honoring `NO_COLOR`). |

Environment: `OCC_AS`, `OCC_LD` and `OCC_CC` override the tools occ runs, `OCC_LINK_WITH_CC=1`
links through `cc` instead of calling `ld` directly, and `OCC_NO_ANIMATION=1` keeps a terminal
display static. When stderr is not a terminal the output is plain text, one line per stage.

## Diagnostics

When something goes wrong, the failing stage turns into `ERR` and the diagnostics follow — each
with its location, the source line, the warning's name and, where the fix is obvious, a hint:

<div align="center">
<img src="docs/assets/diagnostics.svg" alt="occ diagnostics for a file with five mistakes" width="860">
</div>

The first line of every diagnostic keeps GCC's `file:line:col: warning:` format, so editors and
build tools understand it. All warnings are on by default; those marked **UB** flag code whose
behavior the C standard leaves undefined.

<details>
<summary><b>All 27 warnings</b> (<code>occ --warnings</code>)</summary>

| Warning | Detects |
|---|---|
| `unused-variable` | local variable is never used (or only ever assigned) |
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

Removed features are hard errors: implicit `int` and implicit function declarations (removed in
C99), `gets()` (removed in C11) and K&R-style function definitions (removed in C23).

</details>

## How it works

<div align="center">
<img src="docs/assets/pipeline.svg" alt="The occ pipeline: preprocess, compile (parse, optimize, codegen), assemble, link" width="860">
</div>

The parser produces a fully typed AST in which every implicit conversion is an explicit cast, so
the code generator never has to rediscover C's conversion rules. `-o none` is a simple
accumulator/stack machine — obviously correct, and the reference `-o prod` is tested against.
`-o prod` keeps its skeleton and removes its costs:

| | `-o none` | `-o prod` |
|---|---|---|
| Variables | on the stack | scalars in `%rbx`, `%r12`–`%r15`; `double`/`float` in `%xmm12`–`%xmm15` |
| Temporaries | `push`/`pop` | scratch registers `%r8`–`%r11`, `%xmm8`–`%xmm11` |
| Memory access | address in `%rax`, then load | `disp(base, index, scale)` operands, read-modify-write, immediate stores |
| Control flow | test at the loop top | rotated loops, direct `cmp`+`jcc`, jump tables for dense `switch` |
| AST passes | — | constant folding, algebraic identities, strength reduction, dead branches, loop-invariant code motion |
| Cleanup | — | peephole pass |

```text
src/
├── support/   vectors, strings, hash map, source files, diagnostics, timers
├── preproc/   lexer, macro expansion (Prosser hidesets), directives, #if evaluation
├── parse/     declarations, expressions, statements, initializers, types, scopes,
│              constant evaluation, and the static checker behind most warnings
├── opt/       -o prod AST passes: folding and simplification, loop-invariant code motion
├── codegen/   expressions, statements, calls and the SysV ABI, data, registers, peephole
└── driver/    command line, pipeline, live status display, external tools
include/       freestanding headers occ provides (stddef.h, stdarg.h, stdckdint.h, ...)
examples/      small programs to try          tests/   runtime, diagnostic, benchmark, fuzz
```

Why it is built this way — and what went wrong along the way — is written up in
[**THINKPROC.md**](THINKPROC.md).

## Performance

Best of 3 runs of the benchmarks in [`tests/bench`](tests/bench) (`make bench`; all builds print
the same checksum). Shorter is faster.

<div align="center">
<img src="docs/assets/benchmarks.svg" alt="Benchmark times of occ -o none, occ -o prod, gcc -O0 and gcc -O2" width="720">
</div>

`-o prod` beats `gcc -O0` everywhere and is within 20% of `gcc -O2` on four of six benchmarks. The
gap that remains is call-heavy code (`fib`: occ does not inline) and vectorizable loops
(`matmul`). On a big real program the picture is similar: SQLite's `speedtest1` runs in 1.74 s
built with `occ -o prod`, 4.19 s with `-o none` and 0.90 s with `gcc -O2`. Timings come from a
shared machine; expect about ±10% noise.

## Tested on real code

These projects were built with occ at both optimization levels and checked with their own tests
(or byte-for-byte against a GCC build of the same code):

| Project | Size | Result with `-o none` and `-o prod` |
|---|---:|---|
| [SQLite](https://sqlite.org) 3.53 + shell | 307k lines | SQL workload identical to GCC's build; `speedtest1` verification hash identical |
| [Lua](https://www.lua.org) 5.4.9 | 30k lines | interpreter runs; scripts give identical output |
| [Duktape](https://duktape.org) 2.7 | 108k lines | JavaScript test script: identical output |
| [MuJS](https://mujs.com) 1.3.9 | 20k lines | JavaScript test script: identical output |
| [Jim Tcl](https://jim.tcl.tk) 0.83 | 43k lines | its test suite: 5551 of 5729 pass — the same as with GCC |
| [zlib](https://zlib.net) 1.3.2 | 25k lines | `example` passes; `minigzip` output byte-identical to GCC's |
| [bzip2](https://sourceware.org/bzip2/) 1.0.8 | 8k lines | its `make test` samples pass; output byte-identical to GCC's |
| [LZ4](https://lz4.org) 1.10 | 18k lines | levels 1/9/12 byte-identical to GCC's, round trips OK |
| [xxHash](https://xxhash.com) 0.8.3 | 12k lines | sanity test: 49948 vectors pass; all four hashes match |
| [Csmith](https://github.com/csmith-project/csmith) | random | 400 generated programs, no mismatch with GCC |

Every one of these runs found or confirmed something: the Lua build exposed two false-positive
warnings, xxHash a preprocessor bug, and Jim Tcl a missing `-rdynamic` option — all fixed, with
regression tests.

## C23 support

<details open>
<summary>What's in</summary>

- `constexpr` objects, `auto` type inference, `typeof` / `typeof_unqual`
- `nullptr` / `nullptr_t`, `bool` / `true` / `false` as keywords
- Enumerations with a fixed underlying type (`enum e : uint8_t`)
- Attributes: `[[nodiscard]]`, `[[deprecated]]`, `[[fallthrough]]`, `[[maybe_unused]]`,
  `[[noreturn]]`, `[[unsequenced]]`, `[[reproducible]]`
- `#embed`, `#elifdef` / `#elifndef`, `#warning`, `__has_include`, `__has_embed`,
  `__has_c_attribute`, `__VA_OPT__`
- `static_assert` without a message, empty initializers `= {}`
- Binary literals and digit separators (`0b1010'0101`)
- Labels before declarations and at the end of blocks
- Unnamed parameters in definitions, `f()` meaning `f(void)`
- `<stdckdint.h>` checked arithmetic (exact, for every mix of operand types), `unreachable()`,
  `u8` character constants
- Everything from C99/C11 you'd expect: designated initializers, compound literals, flexible array
  members, anonymous structs/unions, `_Generic`, `_Alignas`/`_Alignof`, `_Thread_local`, variadic
  functions, bit-fields, `setjmp`/`longjmp`, struct passing and returning by value (SysV ABI)

</details>

<details>
<summary>What's not (yet)</summary>

- Variable length arrays — rejected with an error (optional since C11)
- `_BitInt(N)`, `_Complex`, decimal floating point
- `long double` is compiled as `double`; `_Atomic` is accepted without atomic semantics (both
  reported by `-Wunsupported`)
- GNU statement expressions and extended `asm` (basic `asm("...")` works)
- Targets other than x86-64 Linux with glibc — by design

</details>

A tour of the features lives in [`examples/c23_tour.c`](examples/c23_tour.c).

## Testing

```sh
make test                   # runtime + diagnostic tests, at -o none and -o prod
tests/run.sh --reference    # validate the runtime tests themselves with GCC
make selfhost               # occ builds occ; stage 1 and 2 must emit identical assembly
make fuzz FUZZ_COUNT=1000   # random UB-free programs, occ vs GCC
tests/fuzz/csmith.py        # Csmith-generated programs, occ vs GCC (needs csmith)
make bench                  # the chart above
```

The diagnostic tests are strict both ways: every expected warning must appear, and every warning
occ prints must be expected. CI runs all of the above on every push.

## Built with Claude

<img src="https://cdn.simpleicons.org/claude/D97757" alt="Claude" width="40" align="left">

occ was designed, written, tested and documented by Claude (Anthropic's AI model), which manages
this repository: every commit, test and page of documentation. The reasoning behind the design,
the bugs found along the way and how they were caught are in [THINKPROC.md](THINKPROC.md).

<br clear="left">

## License

[MIT](LICENSE)

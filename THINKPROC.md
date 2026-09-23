# THINKPROC — how occ was designed

This is a summary of the reasoning behind occ: what the goals were, which
trade-offs were made and why, what went wrong along the way and how it was
caught. It is written after the fact, from the notes kept while building it.

## 1. Reading the brief

The request boiled down to six hard requirements and a handful of soft ones:

| Requirement | What it meant in practice |
|---|---|
| C compiler → x86-64 Linux binaries | ELF executables that run on a normal glibc distribution |
| All common pipeline stages | preprocessor, compiler, assembler, linker — each visible as a stage |
| Pipeline status in the terminal | stage name, CPU time, `OK`/`ERR`, diagnostics with source excerpts |
| Warnings for deprecated / UB-prone code | a real (if small) static checker, not just parse errors |
| `-o none\|prod`, `-n name` | two optimization levels; `prod` must be meaningfully faster |
| Follow C23 | both *accept* C23 and be *written in* C23 |

"Simple" and "KISS" pulled against "production-ready -O2 equivalent" and "must
work on real code". The resolution was: keep the *architecture* simple (a
single typed AST, one code generator, no IR zoo), and spend complexity only
where measurements showed it paid off.

## 2. The big decisions

### Own the front end, borrow the back end's last mile

The preprocessor, parser, type checker, optimizer and code generator are all
occ's own. Assembling and linking are delegated to GNU `as` and `ld`.

Why not write an assembler and linker too? Because the brief asks for the
*stages* to exist and be reported, and an x86 encoder plus an ELF linker that
handles glibc's startup files, PIE relocations, RELRO and TLS is several
thousand lines whose bugs produce silently broken binaries. binutils is
present on every system that has glibc development files, and occ still owns
everything about *what* is linked: it locates `Scrt1.o`, `crti.o`,
`crtbeginS.o` etc. itself and builds the `ld` command line, falling back to
`cc` only if that search fails. Every link produces a PIE with full RELRO and
a non-executable stack, which the status line reports.

### A real preprocessor that can eat glibc

Many hobby compilers ship their own libc headers. occ uses the system's
`/usr/include` unchanged, with only the freestanding headers (`stddef.h`,
`stdarg.h`, `stdbool.h`, `float.h`, `stdckdint.h`, ...) provided by occ in
`include/`. That forced the preprocessor to be correct rather than
approximately correct: Prosser's hideset algorithm for macro expansion,
placemarkers for `##` with empty arguments, `__VA_OPT__`, and the C23
additions (`#embed`, `#elifdef`, `__has_include`, `__has_embed`,
`__has_c_attribute`). The standard's own macro examples are part of the test
suite and produce GCC's output token for token.

occ deliberately does **not** define `__GNUC__`. glibc's headers then take
their portable code paths instead of the GNU-only ones (statement
expressions, inline assembly, compiler builtins), and the few extensions
that remain (`__attribute__`, `__extension__`, `__restrict`, `__inline`) are
simply parsed.

### One typed AST, nothing more

The parser produces a fully typed AST: every implicit conversion is an
explicit cast node and array/function decay is an explicit address-of. The
code generator therefore never has to reason about C's conversion rules — if
a cast is needed, it is in the tree. That single decision removed a whole
class of bugs where "the parser thought it was `long`, codegen thought it was
`int`".

No SSA, no separate IR. For a compiler whose optimizer target was "roughly
-O1/-O2 on typical loops", a tree optimizer plus a smarter code generator
was estimated to get most of the benefit at a fraction of the code. The
benchmarks (below) mostly bear that out.

### Code generation: a stack machine that learned to use registers

`-o none` is a classic accumulator/stack machine: every expression result
ends up in `%rax` (or `%xmm0`), intermediate values are pushed. It is
obviously correct, easy to debug, and it is the reference that `-o prod` is
tested against.

`-o prod` keeps the same skeleton but removes its costs one by one, each
change justified by a benchmark:

1. **Register variables.** Scalar locals whose address is never taken live in
   callee-saved `%rbx`, `%r12`–`%r15`, chosen by a use count weighted by loop
   depth. `double`/`float` locals get `%xmm12`–`%xmm15`, spilled around calls
   because SysV has no callee-saved vector registers.
2. **Scratch temporaries instead of push/pop.** While evaluating the other
   operand has no side effects and makes no calls, an intermediate lives in
   `%r8`–`%r11` or `%xmm8`–`%xmm11` rather than on the stack.
3. **Addressing modes.** `a[i]`, `p->x`, `s.a[i].b` become one
   `disp(base, index, scale)` operand instead of an address computation
   followed by a load.
4. **Read-modify-write and immediates.** `x += 3` on memory becomes one
   `addl $3, mem`; constant stores do not go through `%rax`.
5. **Rotated loops and jump tables.** Loop conditions are tested at the
   bottom; dense `switch` statements use a `.rodata` jump table.
6. **Branches on comparisons.** `if (a < b)` becomes `cmp` + `jge` directly,
   including NaN-correct floating-point comparisons (`ucomisd` + parity).
7. **A peephole pass** for what is left: push/pop pairs, jumps to the next
   label, store-then-reload, `cmp $0` → `test`, inverted `jCC`-over-`jmp`.

The AST optimizer runs before all of this: constant folding (sharing its
evaluator with the parser's constant-expression code, so the two can never
disagree), algebraic identities, strength reduction, dead-branch removal and
loop-invariant code motion of address arithmetic.

### Warnings are part of the product

The brief asked for warnings about deprecated and UB-prone code, so the
checker was designed around *undefined behavior a compiler can see
statically*: constant division by zero, over-wide shifts, out-of-bounds
constant indexes, returning a local's address, falling off the end of a
non-void function, `printf`/`scanf` format mismatches, reading a variable
that is never written, and so on. Deprecated and removed features are
flagged too: `gets()` and K&R-style definitions are errors (both were removed
from the language), while `[[deprecated]]` entities and the obsolescent
`_Noreturn` produce warnings. Every warning has a name, can be disabled with
`-Wno-<name>`, and is listed by `occ --warnings`.

Diagnostics look like GCC/Clang's (file:line:col, source excerpt, caret and
underline) because that is what editors and people already parse. Errors
recover at statement/declaration boundaries so one mistake does not hide the
next five.

### Timing that means something

"How long the stage took" is measured as **CPU time**, as asked: the
compiler's own stages use `CLOCK_PROCESS_CPUTIME_ID`, and the external
assembler/linker are measured with `wait4()`'s `rusage` of the child. Wall
time is shown next to it, since the two differ exactly when something
interesting happens (I/O, a cold cache).

## 3. How correctness was established

Trusting a compiler requires more than "the tests pass". occ uses four
independent layers:

1. **Self-checking runtime tests** (`tests/cases`, ~650 checks), each built
   at both optimization levels. The runner can also build them with GCC
   (`--reference gcc`), which caught several tests that asserted the wrong
   thing — the test was wrong, not the compiler.
2. **Diagnostic tests** (`tests/diag`) that pin down which warnings and
   errors appear, and that clean code produces none.
3. **Self-hosting.** occ compiles itself; that compiler compiles itself
   again; the two generate byte-identical assembly (`make selfhost`). A
   compiler that miscompiles itself rarely reaches a fixpoint, so this is a
   strong end-to-end check of ~13k lines of real C.
4. **Differential fuzzing** (`tests/fuzz/fuzz.py`). Random programs free of
   undefined behavior — mixed integer widths, arrays, structs, pointers,
   loops, `switch`, calls — are compiled by occ and GCC, and their outputs
   compared.

The fuzzer earned its place immediately: its very first run flagged 67 of 150
programs at `-o prod`. All 67 were one bug. Loop-invariant code motion wraps a
loop into a new block `{ hoisted temps; loop }` but left the loop's `next`
pointer intact, so *the statement after the loop was emitted twice*. None of
the hand-written tests noticed, because in every one of them the loop was
followed by a `return`, which turned the duplicate into dead code. That is
exactly the kind of blind spot hand-written tests have and random ones don't.
The fix is one line; the regression test is written so a duplicated statement
is observable.

Other bugs worth remembering, each of which led to a design rule:

- `r = setjmp(env);` crashed at `-o none` because the address of `r` was
  pushed before the call and `longjmp` returns with a different stack
  picture. *Rule:* for stores to plain variables, evaluate the value first.
- Parser-generated label numbers collided with code-generator labels.
  *Rule:* the two namespaces are spelled differently (`.L7` vs `.L.cg.7`).
- `_Generic` could not tell `char` from `signed char`. *Rule:* they are
  distinct types that merely share a representation.
- `-Wformat` missed `scanf` because glibc maps it to `__isoc23_scanf`.
  *Rule:* check the function a call resolves to, then normalize known
  aliases.

## 4. The second round: real code and a live display

The first round's tests were all written by the same mind that wrote the
compiler, so a second round went looking for bugs somewhere else: other
people's code. Lua, SQLite (307k lines including its shell), Duktape, MuJS,
Jim Tcl, zlib, bzip2, LZ4 and xxHash were built with occ at both levels and
checked with their own test suites, or byte-for-byte against GCC builds;
500 Csmith programs and an AddressSanitizer build of occ itself ran
alongside. It paid off in exactly the places hand-written tests are weak:

- **Mixed-sign overflow builtins.** `ckd_add(&unsigned_long, -1, 0)` said
  "no overflow". The operands had been converted to the result's type
  first; the fix computes the exact 128-bit result. It is now checked
  against GCC for all 1536 combinations of operand and result types.
- **False-positive warnings** from Lua: `(x = f()) >= y` is not "set but
  not used", and a promoted `unsigned char` compared with `size_t` is not a
  sign problem. The diagnostic tests became strict both ways, so any new
  false positive now fails the suite.
- **`__has_c_attribute` produced by a macro** inside `#if` (xxHash) was
  rejected; such operators are now evaluated after expansion.
- **`-rdynamic`** was missing, which Jim Tcl's plugin tests need.
- **Bit-field promotion** (Csmith): an `unsigned int x : 14` promotes to
  `int`, not `unsigned int`, because `int` holds all its values. occ used
  the declared type, which turned a comparison with a negative `short`
  upside down. One of 500 Csmith programs caught it.

The live display followed the same rule as the rest: it must never cost
correctness or machine-readability. The spinner runs on a helper thread only
while a stage runs and only on a terminal; the first line of every
diagnostic keeps GCC's format; and anything that is not a terminal gets the
plain, line-per-stage output the test suite checks.

## 5. Things deliberately left out

Chosen to keep the compiler honest about what it does rather than half-doing
it:

- **VLAs** — rejected with a clear error (optional in C11+, and rarely what
  you want).
- **`long double`** is treated as `double`, and `_Atomic` is accepted without
  atomic semantics; occ says so with `-Wunsupported`. `_Complex` and
  `_BitInt` are rejected.
- **GNU statement expressions and extended `asm`** — only basic `asm("...")`.
- **Other targets** — x86-64 Linux with glibc only, by design.

## 6. If this were to go further

- A small SSA-based register allocator would close most of the remaining gap
  to `gcc -O2` on call-heavy code like `fib`.
- Inlining of small `static` functions.
- `_BitInt(N)` for the remaining C23 surface.

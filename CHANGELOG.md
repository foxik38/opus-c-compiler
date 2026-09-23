# Changelog

All notable changes to occ are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and occ uses
[semantic versioning](https://semver.org/).

## [Unreleased]

### Changed
- New image theme (pitch black, grays, pastels and dark blue), a new logo, and a smoother
  animated terminal demo in the README.
- `occ --version` prints `occ 1.0.0`.

### Added
- A project website in `docs/` (GitHub Pages), a social preview image, contribution
  guidelines, a security policy, and issue and pull request templates.

## [1.0.0] — 2026-09-23

The first release: a self-hosting C23 compiler for x86-64 Linux.

### Added
- Pipeline: preprocessor, parser with a fully typed AST, AST optimizer, x86-64 code generator
  (SysV ABI); GNU `as` and `ld` driven directly (PIE, full RELRO, NX stack).
- Command line: `-o none|prod`, `-n`, `-E`/`-S`/`-c`, `--run`, `--keep`, `-I`/`-D`/`-U`,
  `-l`/`-L`, `--static`, `-rdynamic`, warning control (`-w`, `-Werror`, `-W<name>`).
- Live pipeline display on terminals: per-stage status, CPU and wall time, spinner and a
  time breakdown; plain GCC-compatible output everywhere else.
- 27 named warnings aimed at undefined behavior and deprecated features, with `help:` hints.
- C23: `constexpr`, `auto`, `typeof`, `nullptr`, `bool`, `#embed`, attributes,
  `<stdckdint.h>`, typed enums, digit separators; `#pragma pack` and `__attribute__((packed))`.
- `-o prod`: register allocation, scratch-register temporaries, addressing modes, constant
  folding, strength reduction, loop-invariant code motion, jump tables, peephole pass.
- Tests: runtime and diagnostic suites, self-host fixpoint, differential fuzzing against GCC
  (own generator and Csmith), benchmarks; verified on SQLite, Lua, Duktape, MuJS, Jim Tcl, zlib,
  bzip2, LZ4 and xxHash.

[Unreleased]: https://github.com/foxik38/opus-c-compiler/compare/9363fdc...HEAD
[1.0.0]: https://github.com/foxik38/opus-c-compiler/tree/9363fdc

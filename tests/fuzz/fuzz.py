#!/usr/bin/env python3
"""Differential fuzzer: random C programs compiled by occ and a reference compiler.

    tests/fuzz/fuzz.py [--count N] [--seed S] [--occ ./occ] [--ref gcc]

Each generated program is free of undefined behavior: arithmetic that could
overflow is done on unsigned types, divisors are forced non-zero and shift
counts are masked. Programs mix integer widths, globals, arrays, structs,
pointers, loops with break/continue, switch statements and calls to generated
helper functions, then print every variable. A mismatch between occ (at -o none and -o prod) and the reference
compiler is reported with the offending program saved for inspection.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

TYPES = ["uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t"]
UNSIGNED = [t for t in TYPES if t.startswith("u")]


class Gen:
    def __init__(self, rng):
        self.rng = rng
        self.vars = []  # (name, type)
        self.globals = []  # (name, type)
        self.funcs = 0  # helpers defined so far may be called
        self.in_loop = False
        self.in_main = True  # arr, s and p are locals of main

    def pick_var(self):
        return self.rng.choice(self.vars)

    def expr(self, depth=0):
        r = self.rng
        if depth > 4 or r.random() < 0.25:
            choice = r.random()
            if choice < 0.42:
                name, _ = self.pick_var()
                return name
            if choice < 0.5 and self.globals:
                return r.choice(self.globals)[0]
            if self.in_main and choice < 0.65:
                return f"arr[{self.index()}]"
            if self.in_main and choice < 0.75:
                return f"s.{r.choice(['a', 'b', 'c'])}"
            if self.in_main and choice < 0.8:
                return f"(*p)"
            return str(r.choice([0, 1, 2, 3, 7, 8, 15, 16, 31, 100, 255, 256, 1000, 65535, 65536, 0x7fffffff,
                                 0x80000000, 0xffffffff, -1, -2, -128, 127]))
        a = self.expr(depth + 1)
        b = self.expr(depth + 1)
        op = r.choice(["+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^", "<", "<=", ">", ">=", "==", "!=",
                       "&&", "||", "?:", "cast", "neg", "not", "call", "compl"])
        u = r.choice(UNSIGNED)
        if op == "*" and u in ("uint8_t", "uint16_t"):
            u = "uint32_t"  # narrow operands promote to int, whose overflow is UB
        if op in "+-*":
            return f"(({u})({a}) {op} ({u})({b}))"
        if op in ("/", "%"):
            return f"(({u})({a}) {op} (({u})({b}) ? ({u})({b}) : 1u))"
        if op in ("<<", ">>"):
            return f"(({u})({a}) {op} (({b}) & 7))"
        if op in ("&", "|", "^", "<", "<=", ">", ">=", "==", "!=", "&&", "||"):
            return f"(({a}) {op} ({b}))"
        if op == "?:":
            return f"(({self.expr(depth + 1)}) ? ({a}) : ({b}))"
        if op == "cast":
            return f"(({r.choice(TYPES)})({a}))"
        if op == "neg":
            return f"(-({u})({a}))"
        if op == "compl":
            return f"(~({u})({a}))"
        if op == "not":
            return f"(!({a}))"
        if self.funcs and r.random() < 0.5:
            return f"f{r.randrange(self.funcs)}(({a}), ({b}), ({self.expr(depth + 1)}))"
        return f"mix(({a}), ({b}))"

    def index(self):
        name, _ = self.pick_var()
        return f"({name}) & 7"

    def stmt(self, depth=0):
        r = self.rng
        choice = r.random()
        name, ty = self.pick_var()
        if depth < 2 and choice < 0.12:
            n = r.randint(1, 6)
            outer, self.in_loop = self.in_loop, True
            body = "\n".join(self.stmt(depth + 1) for _ in range(r.randint(1, 3)))
            self.in_loop = outer
            if r.random() < 0.3:  # the same loop as while or do-while
                kind = r.choice(["while", "do"])
                head = f"int i{depth} = 0;"
                if kind == "while":
                    return f"{{ {head} while (i{depth}++ < {n}) {{\n{body}\n}} }}"
                return f"{{ {head} do {{\n{body}\n}} while (++i{depth} < {n}); }}"
            return f"for (int i{depth} = 0; i{depth} < {n}; i{depth}++) {{\n{body}\n}}"
        if self.in_loop and choice < 0.14:
            return f"if (({self.expr()}) & 1) {r.choice(['break', 'continue'])};"
        if depth < 2 and choice < 0.2:
            body = "\n".join(self.stmt(depth + 1) for _ in range(r.randint(1, 3)))
            other = "\n".join(self.stmt(depth + 1) for _ in range(r.randint(0, 2)))
            return f"if ({self.expr()}) {{\n{body}\n}} else {{\n{other}\n}}"
        if depth < 2 and choice < 0.25:
            cases = []
            for k in r.sample(range(0, 8), r.randint(1, 5)):
                cases.append(f"case {k}: {self.stmt(depth + 1)} break;")
            return f"switch (({self.expr()}) & 7) {{\n" + "\n".join(cases) + f"\ndefault: {self.stmt(depth + 1)}\n}}"
        if choice < 0.3 and self.globals:
            gname, gty = r.choice(self.globals)
            return f"{gname} = ({gty})({self.expr()});"
        if choice < 0.35:
            return f"arr[{self.index()}] = ({self.expr()});"
        if choice < 0.42:
            return f"s.{r.choice(['a', 'b', 'c'])} = ({self.expr()});"
        if choice < 0.47:
            return f"*p = ({self.expr()});"
        if choice < 0.52:
            return f"p = &arr[{self.index()}];"
        if choice < 0.6:
            op = r.choice(["+=", "-=", "^=", "|=", "&="])
            u = "uint64_t" if ty.endswith("64_t") else "uint32_t"
            return f"{name} = ({ty})(({u}){name} {op[0]} ({u})({self.expr()}));"
        if choice < 0.65:
            return f"{name}++;" if ty in UNSIGNED else f"{name} = ({ty})((uint64_t){name} + 1);"
        return f"{name} = ({ty})({self.expr()});"

    def helper(self, k):
        # uint64_t fK(uint32_t a, int64_t b, uint16_t c) over its parameters.
        saved, self.vars = self.vars, [("a", "uint32_t"), ("b", "int64_t"), ("c", "uint16_t")]
        self.in_main = False
        body = self.expr(2)
        self.vars, self.in_main = saved, True
        return f"static uint64_t f{k}(uint32_t a, int64_t b, uint16_t c) {{ return (uint64_t)({body}); }}"

    def program(self):
        r = self.rng
        for i in range(r.randint(0, 3)):
            self.globals.append((f"g{i}", r.choice(TYPES)))
        gdecls = "\n".join(f"static {t} {n} = ({t}){r.randint(-50, 5000)};" for n, t in self.globals)
        helpers = []
        for k in range(r.randint(0, 3)):
            helpers.append(self.helper(k))
            self.funcs = k + 1
        decls = []
        for i in range(r.randint(3, 8)):
            ty = r.choice(TYPES)
            name = f"v{i}"
            self.vars.append((name, ty))
            decls.append(f"  {ty} {name} = ({ty}){r.randint(-1000, 100000)};")
        body = "\n".join(self.stmt() for _ in range(r.randint(8, 25)))
        prints = "\n".join(f'  printf("{n}=%llu\\n", (unsigned long long)(uint64_t){n});'
                           for n, _ in self.vars + self.globals)
        return f"""#include <stdint.h>
#include <stdio.h>

static uint64_t mix(uint64_t a, uint64_t b) {{ return (a * 31u) ^ (b >> 3); }}
{gdecls}
{chr(10).join(helpers)}

int main(void) {{
  uint32_t arr[8] = {{1, 2, 3, 4, 5, 6, 7, 8}};
  struct {{ uint8_t a; int32_t b; uint64_t c; }} s = {{9, -10, 11}};
  uint32_t *p = &arr[3];
{chr(10).join(decls)}
{body}
{prints}
  for (int k = 0; k < 8; k++) printf("arr[%d]=%u\\n", k, arr[k]);
  printf("s=%u %d %llu p=%d\\n", s.a, s.b, (unsigned long long)s.c, (int)(p - arr));
  return 0;
}}
"""


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=30, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--occ", default="./occ")
    ap.add_argument("--ref", default="gcc")
    args = ap.parse_args()

    seed = args.seed if args.seed is not None else random.randrange(1 << 30)
    print(f"fuzzing with seed {seed}")
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n in range(args.count):
            rng = random.Random(seed + n)
            src = os.path.join(tmp, "p.c")
            with open(src, "w") as f:
                f.write(Gen(rng).program())

            ref_exe = os.path.join(tmp, "ref")
            r = run([args.ref, "-std=c2x", "-w", "-O0", "-o", ref_exe, src])
            if r.returncode != 0:
                print(f"[{n}] reference compiler failed:\n{r.stderr}")
                continue
            expected = run([ref_exe]).stdout

            for mode in ("none", "prod"):
                exe = os.path.join(tmp, f"occ-{mode}")
                r = run([args.occ, "-q", "-w", "-o", mode, "-n", exe, src])
                got = r.stderr if r.returncode != 0 else run([exe]).stdout
                if got != expected:
                    failures += 1
                    keep = f"tests/out/fuzz-fail-{seed + n}-{mode}.c"
                    os.makedirs("tests/out", exist_ok=True)
                    with open(src) as a, open(keep, "w") as b:
                        b.write(a.read())
                    print(f"[{n}] MISMATCH at -o {mode}: saved {keep}")
                    break
            if (n + 1) % 25 == 0:
                print(f"  {n + 1} programs, {failures} failures")
    print(f"done: {args.count} programs, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

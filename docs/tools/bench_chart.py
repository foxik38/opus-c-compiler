#!/usr/bin/env python3
"""Draws docs/assets/benchmarks.svg from the table printed by `make bench`.

    make bench | tee bench.txt && docs/tools/bench_chart.py bench.txt docs/assets/benchmarks.svg

Each benchmark gets a group of bars (occ -o none, occ -o prod, gcc -O0,
gcc -O2), scaled to the slowest build of that benchmark, with the seconds
printed next to every bar. Shorter is faster.
"""
import sys

COLORS = {"occ -o none": "#7f849c", "occ -o prod": "#89b4fa", "gcc -O0": "#f9e2af", "gcc -O2": "#a6e3a1"}


def parse(path):
    rows, header = [], None
    for line in open(path):
        cells = [c.strip() for c in line.split("|")]
        if len(cells) < 3 or set(line.strip()) <= set("-| "):
            continue
        if cells[0] == "benchmark":
            header = cells[1:]
            continue
        if header:
            rows.append((cells[0], [float(c.rstrip("s")) for c in cells[1:]]))
    return header, rows


def main():
    header, rows = parse(sys.argv[1])
    bar_h, gap, group_gap, left, bar_w = 13, 3, 20, 120, 520
    group_h = len(header) * (bar_h + gap) + group_gap
    legend_h = 44
    height = legend_h + len(rows) * group_h + 16
    width = left + bar_w + 110
    font = "font-family=\"'JetBrains Mono','Fira Code','DejaVu Sans Mono',Menlo,Consolas,monospace\""
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
           f'<rect width="{width}" height="{height}" rx="14" fill="#1e1e2e"/>']
    x = 24
    for name in header:
        out.append(f'<rect x="{x}" y="18" width="14" height="14" rx="3" fill="{COLORS.get(name, "#cdd6f4")}"/>')
        out.append(f'<text x="{x + 20}" y="30" {font} font-size="13" fill="#cdd6f4">{name}</text>')
        x += 20 + len(name) * 8 + 28
    y = legend_h + 8
    for bench, times in rows:
        slowest = max(times)
        out.append(f'<text x="24" y="{y + len(header) * (bar_h + gap) / 2 + 4}" {font} font-size="14" '
                   f'font-weight="700" fill="#cdd6f4">{bench}</text>')
        for name, t in zip(header, times):
            w = max(2, t / slowest * bar_w)
            out.append(f'<rect x="{left}" y="{y}" width="{w:.1f}" height="{bar_h}" rx="3" fill="{COLORS.get(name, "#cdd6f4")}"/>')
            out.append(f'<text x="{left + w + 8:.1f}" y="{y + bar_h - 2}" {font} font-size="11.5" fill="#a6adc8">{t:.3f} s</text>')
            y += bar_h + gap
        y += group_gap
    out.append("</svg>")
    with open(sys.argv[2], "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()

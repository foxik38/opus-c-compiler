#!/usr/bin/env python3
"""Renders captured terminal output (with ANSI colors) as an SVG image.

    docs/tools/render_terminal.py capture.txt out.svg [--title T] [--command CMD] [--animate]

The capture is what a terminal received, e.g. from
    script -qfc "occ -o prod life.c" capture.txt
Carriage returns and "erase line" sequences are replayed, so a live display
(spinners redrawn in place) ends up as its final text. With --animate, the
command is typed out and the lines appear one after another, looping.
"""
import argparse
import html
import re

PALETTE_16 = ["#1e1e2e", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#bac2de",
              "#585b70", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#cdd6f4"]
FOREGROUND = "#cdd6f4"
BACKGROUND = "#1e1e2e"


def color_256(n):
    if n < 16:
        return PALETTE_16[n]
    if n < 232:
        n -= 16
        steps = [0, 95, 135, 175, 215, 255]
        return "#%02x%02x%02x" % (steps[n // 36], steps[n // 6 % 6], steps[n % 6])
    v = 8 + (n - 232) * 10
    return "#%02x%02x%02x" % (v, v, v)


def parse(text):
    """Returns the final screen: a list of lines, each a list of (char, style)."""
    text = re.sub(r"^Script started.*?\r?\n", "", text)
    text = re.sub(r"\r?\n?Script done.*$", "", text, flags=re.S)
    lines, cur, col = [], [], 0
    style = {"fg": None, "bold": False, "dim": False}
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "\x1b":
            m = re.match(r"\x1b\[([0-9;]*)([A-Za-z])", text[i:])
            if m:
                params, cmd = m.group(1), m.group(2)
                if cmd == "m":
                    style = apply_sgr(style, params)
                elif cmd == "K":
                    del cur[col:]
                i += len(m.group(0))
                continue
        if ch == "\r":
            col = 0
        elif ch == "\n":
            lines.append(cur)
            cur, col = [], 0
        else:
            cell = (ch, dict(style))
            if col < len(cur):
                cur[col] = cell
            else:
                cur.append(cell)
            col += 1
        i += 1
    if cur:
        lines.append(cur)
    while lines and not lines[-1]:
        lines.pop()
    return lines


def apply_sgr(style, params):
    codes = [int(p) for p in params.split(";") if p] or [0]
    style = dict(style)
    k = 0
    while k < len(codes):
        c = codes[k]
        if c == 0:
            style = {"fg": None, "bold": False, "dim": False}
        elif c == 1:
            style["bold"] = True
        elif c == 2:
            style["dim"] = True
        elif 30 <= c <= 37:
            style["fg"] = PALETTE_16[c - 30 + (8 if style["bold"] else 0)]
        elif 90 <= c <= 97:
            style["fg"] = PALETTE_16[c - 90 + 8]
        elif c == 38 and k + 2 < len(codes) and codes[k + 1] == 5:
            style["fg"] = color_256(codes[k + 2])
            k += 2
        elif c == 39:
            style["fg"] = None
        k += 1
    return style


def spans(line):
    """Groups a line into runs of equal style."""
    runs = []
    for ch, st in line:
        key = (st["fg"], st["bold"], st["dim"])
        if runs and runs[-1][0] == key:
            runs[-1][1].append(ch)
        else:
            runs.append((key, [ch]))
    return [(key, "".join(chars)) for key, chars in runs]


def render(lines, title, command, animate):
    char_w, line_h, pad = 8.4, 19, 18
    top = 44
    rows = ([("$ ", command)] if command else []) + lines
    width = int(max(len(r[1]) + 2 if isinstance(r, tuple) else len(r) for r in rows) * char_w + 2 * pad)
    width = max(width, 520)
    height = top + len(rows) * line_h + pad
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" xml:space="preserve" width="{width}" height="{height}" viewBox="0 0 {width} {height}">']
    out.append("<style>")
    out.append("text{font-family:'JetBrains Mono','Fira Code','DejaVu Sans Mono',Menlo,Consolas,monospace;"
               "font-size:14px;white-space:pre;fill:%s}" % FOREGROUND)
    out.append(".b{font-weight:700}.d{opacity:.62}")
    if animate:
        n = len(rows)
        cycle = 2.2 + 0.28 * n + 4.0
        for k in range(n):
            start = (0.0 if (command and k == 0) else 1.6 + 0.28 * k) / cycle * 100
            out.append(f"@keyframes l{k}{{0%,{start:.2f}%{{opacity:0}}{start + 0.01:.2f}%,97%{{opacity:1}}100%{{opacity:0}}}}")
            out.append(f".l{k}{{animation:l{k} {cycle:.2f}s steps(1) infinite}}")
        if command:
            cw = len(command) * char_w
            type_end = 1.4 / cycle * 100
            out.append(f"@keyframes type{{0%{{width:0}}{type_end:.2f}%,100%{{width:{cw:.1f}px}}}}")
            out.append(f".typing{{animation:type {cycle:.2f}s steps({len(command)}) infinite}}")
    out.append("</style>")
    out.append(f'<rect width="{width}" height="{height}" rx="10" fill="{BACKGROUND}"/>')
    out.append(f'<rect width="{width}" height="30" rx="10" fill="#181825"/><rect y="20" width="{width}" height="10" fill="#181825"/>')
    for k, color in enumerate(["#f38ba8", "#f9e2af", "#a6e3a1"]):
        out.append(f'<circle cx="{18 + 20 * k}" cy="15" r="6" fill="{color}"/>')
    if title:
        out.append(f'<text x="{width / 2}" y="20" text-anchor="middle" class="d" style="font-size:12px">{html.escape(title)}</text>')

    y = top
    for k, row in enumerate(rows):
        cls = f' class="l{k}"' if animate else ""
        if command and k == 0:
            prompt_x = pad + 2 * char_w
            clip = ""
            if animate:
                out.append(f'<defs><clipPath id="c"><rect class="typing" x="{prompt_x}" y="{y - 15}" height="{line_h}" width="0"/></clipPath></defs>')
                clip = ' clip-path="url(#c)"'
            out.append(f'<text x="{pad}" y="{y}" style="fill:#a6e3a1" class="b">$</text>')
            out.append(f'<text x="{prompt_x}" y="{y}"{clip}>{html.escape(command)}</text>')
        else:
            parts = []
            col = 0
            for (fg, bold, dim), s in spans(row):
                # Every run starts at its exact column: glyphs from fallback
                # fonts (box drawing, check marks) are not always one cell wide.
                attrs = [f'x="{pad + col * char_w:.1f}"']
                col += len(s)
                classes = ("b " if bold else "") + ("d" if dim else "")
                if classes.strip():
                    attrs.append(f'class="{classes.strip()}"')
                if fg:
                    attrs.append(f'style="fill:{fg}"')
                parts.append(f'<tspan {" ".join(attrs)}>{html.escape(s)}</tspan>')
            out.append(f'<text x="{pad}" y="{y}"{cls}>{"".join(parts)}</text>')
        y += line_h
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("output")
    ap.add_argument("--title", default="")
    ap.add_argument("--command", default="")
    ap.add_argument("--animate", action="store_true")
    args = ap.parse_args()
    with open(args.capture, encoding="utf-8", errors="replace", newline="") as f:  # keep \r
        lines = parse(f.read())
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(render(lines, args.title, args.command, args.animate))


if __name__ == "__main__":
    main()

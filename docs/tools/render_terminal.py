#!/usr/bin/env python3
"""Renders captured terminal output (with ANSI colors) as an SVG image.

    docs/tools/render_terminal.py capture.txt out.svg [--title T] [--command CMD] [--animate]

The capture is what a terminal received, e.g. from
    script -qfc "occ -o prod life.c" capture.txt
Carriage returns and "erase line" sequences are replayed, so a live display
(spinners redrawn in place) ends up as its final text. With --animate, the
command is typed out and every line sweeps in smoothly from the left, one
after another, looping (smoother than a real terminal, on purpose).
"""
import argparse
import html
import re

# Pitch black, grays and white, with pastel accents (the occ image theme).
PALETTE_16 = ["#0a0a0a", "#ffadc6", "#b5f0c8", "#ffe3a8", "#a8c8ff", "#d4c1ff", "#a6ecec", "#d4d4d4",
              "#525252", "#ffadc6", "#b5f0c8", "#ffe3a8", "#a8c8ff", "#d4c1ff", "#a6ecec", "#fafafa"]
# occ's stage colors (256-color codes) mapped to the same pastels.
STAGE_COLORS = {177: "#d4c1ff", 75: "#a8c8ff", 80: "#a6ecec", 215: "#ffd1a8"}
FOREGROUND = "#f5f5f5"
BACKGROUND = "#000000"
CHROME = "#0d0d0d"
BORDER = "#262626"


def color_256(n):
    if n in STAGE_COLORS:
        return STAGE_COLORS[n]
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


def line_text(row):
    return "".join(ch for ch, _ in row)


def render(lines, title, command, animate):
    char_w, line_h, pad = 8.4, 20, 22
    top = 56
    rows = ([None] if command else []) + lines
    longest = max([len(command) + 2] + [len(r) for r in lines])
    width = max(int(longest * char_w + 2 * pad), 560)
    height = top + len(rows) * line_h + pad
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" xml:space="preserve" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}">']
    out.append("<style>")
    out.append("text{font-family:'JetBrains Mono','Fira Code','DejaVu Sans Mono',Menlo,Consolas,monospace;"
               "font-size:14px;white-space:pre;fill:%s}" % FOREGROUND)
    out.append(".b{font-weight:700}.d{opacity:.55}")

    # Smooth animation: every line sweeps in from the left with an ease-out
    # curve, one after another; the whole screen fades out and the loop restarts.
    timeline = []
    if animate:
        t = 0.3
        for k, row in enumerate(rows):
            n = len(command) + 2 if row is None else max(len(row), 1)
            dur = 1.1 if row is None else min(0.18 + n * 0.0045, 0.55)
            timeline.append((t, dur))
            t += dur + (0.35 if row is None else 0.07)
        hold = 3.5
        cycle = t + hold
        for k, (start, dur) in enumerate(timeline):
            a, b = start / cycle * 100, (start + dur) / cycle * 100
            out.append(f"@keyframes s{k}{{0%,{a:.2f}%{{transform:scaleX(0)}}{b:.2f}%,100%{{transform:scaleX(1)}}}}")
            ease = "cubic-bezier(.45,0,.55,1)" if rows[k] is None else "cubic-bezier(.2,.8,.2,1)"
            out.append(f".s{k}{{transform-box:fill-box;transform-origin:0 0;"
                       f"animation:s{k} {cycle:.2f}s {ease} infinite}}")
        fade = (cycle - 0.6) / cycle * 100
        out.append(f"@keyframes all{{0%,{fade:.2f}%{{opacity:1}}100%{{opacity:0}}}}")
        out.append(f".all{{animation:all {cycle:.2f}s ease-in infinite}}")
        caret_end = (timeline[0][0] + timeline[0][1]) / cycle * 100 if command else 0
        out.append(f"@keyframes caret{{0%,{caret_end:.2f}%{{opacity:1}}{caret_end + 0.1:.2f}%,100%{{opacity:0}}}}")
        out.append(f".caret{{animation:caret {cycle:.2f}s steps(1) infinite}}")
    out.append("</style>")

    # Window: pitch black body, a slightly lighter title bar, a hairline border.
    out.append(f'<rect x="0.5" y="0.5" width="{width - 1}" height="{height - 1}" rx="12" fill="{BACKGROUND}" '
               f'stroke="{BORDER}"/>')
    out.append(f'<path d="M0.5 12.5a12 12 0 0 1 12-12h{width - 25}a12 12 0 0 1 12 12v22h-{width - 1}z" fill="{CHROME}"/>')
    out.append(f'<line x1="0.5" y1="34.5" x2="{width - 0.5}" y2="34.5" stroke="{BORDER}"/>')
    for k, color in enumerate(["#3a3a3a", "#3a3a3a", "#3a3a3a"]):
        out.append(f'<circle cx="{20 + 18 * k}" cy="17.5" r="5.5" fill="{color}"/>')
    if title:
        out.append(f'<text x="{width / 2}" y="22" text-anchor="middle" style="font-size:12px;fill:#8a8a8a">'
                   f'{html.escape(title)}</text>')

    if animate:
        out.append("<defs>")
        y = top
        for k in range(len(rows)):
            out.append(f'<clipPath id="c{k}"><rect class="s{k}" x="{pad - 2}" y="{y - 15}" width="{width - 2 * pad + 4}" '
                       f'height="{line_h}"/></clipPath>')
            y += line_h
        out.append("</defs>")
        out.append('<g class="all">')

    y = top
    for k, row in enumerate(rows):
        clip = f' clip-path="url(#c{k})"' if animate else ""
        if row is None:
            out.append(f'<g{clip}><text x="{pad}" y="{y}" class="b" style="fill:#a8c8ff">$</text>'
                       f'<text x="{pad + 2 * char_w}" y="{y}">{html.escape(command)}</text></g>')
            if animate:
                cx = pad + (len(command) + 2) * char_w + 2
                out.append(f'<rect class="caret" x="{cx:.1f}" y="{y - 13}" width="8" height="16" fill="#a8c8ff" opacity=".8"/>')
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
            out.append(f'<text x="{pad}" y="{y}"{clip}>{"".join(parts)}</text>')
        y += line_h
    if animate:
        out.append("</g>")
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

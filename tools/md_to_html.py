#!/usr/bin/env python3
"""Minimal Markdown (headings/tables/fences) to a printable HTML file."""
from __future__ import annotations

import argparse
import html
import re
from pathlib import Path


def inline(s: str) -> str:
    s = html.escape(s)
    s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    return s


def convert(src: str) -> str:
    lines = src.splitlines()
    out: list[str] = []
    i = 0
    in_code = False
    code_buf: list[str] = []
    table_rows: list[str] = []

    def flush_table() -> None:
        if not table_rows:
            return
        out.append("<table>")
        for ri, row in enumerate(table_rows):
            cells = [c.strip() for c in row.strip("|").split("|")]
            if ri == 1 and all(re.match(r"^:?-+:?$", c.replace(" ", "")) for c in cells):
                continue
            tag = "th" if ri == 0 else "td"
            out.append(
                "<tr>" + "".join(f"<{tag}>{inline(c)}</{tag}>" for c in cells) + "</tr>"
            )
        out.append("</table>")
        table_rows.clear()

    while i < len(lines):
        line = lines[i]
        if line.startswith("```"):
            if in_code:
                out.append("<pre>" + html.escape("\n".join(code_buf)) + "</pre>")
                code_buf = []
                in_code = False
            else:
                flush_table()
                in_code = True
            i += 1
            continue
        if in_code:
            code_buf.append(line)
            i += 1
            continue
        if line.startswith("|"):
            table_rows.append(line)
            i += 1
            continue
        flush_table()
        if line.startswith("# "):
            out.append("<h1>" + inline(line[2:]) + "</h1>")
        elif line.startswith("## "):
            out.append("<h2>" + inline(line[3:]) + "</h2>")
        elif line.startswith("### "):
            out.append("<h3>" + inline(line[4:]) + "</h3>")
        elif line.strip() == "---":
            out.append("<hr>")
        elif line.startswith("- "):
            items = []
            while i < len(lines) and lines[i].startswith("- "):
                items.append("<li>" + inline(lines[i][2:]) + "</li>")
                i += 1
            out.append("<ul>" + "".join(items) + "</ul>")
            continue
        elif re.match(r"^\d+\. ", line):
            items = []
            while i < len(lines) and re.match(r"^\d+\. ", lines[i]):
                items.append("<li>" + inline(re.sub(r"^\d+\. ", "", lines[i])) + "</li>")
                i += 1
            out.append("<ol>" + "".join(items) + "</ol>")
            continue
        elif line.strip():
            out.append("<p>" + inline(line) + "</p>")
        i += 1
    flush_table()
    if in_code:
        out.append("<pre>" + html.escape("\n".join(code_buf)) + "</pre>")
    return "\n".join(out)


CSS = """
@page { size: A4; margin: 14mm; }
body { font-family: 'Microsoft YaHei', 'PingFang SC', sans-serif; font-size: 10.5pt;
       line-height: 1.4; color:#1a1a1a; max-width: 190mm; margin: 0 auto; padding: 8px 12px 20px; }
h1 { font-size: 18pt; margin: 0 0 8px; }
h2 { font-size: 13pt; margin: 16px 0 6px; border-bottom: 1px solid #ccc; }
h3 { font-size: 11.5pt; margin: 12px 0 4px; }
p { margin: 4px 0; }
pre { font-family: Consolas, 'Cascadia Mono', monospace; font-size: 8pt; background:#f6f6f6;
      border:1px solid #ddd; padding:8px; white-space:pre-wrap; word-break:break-all; }
table { border-collapse: collapse; width:100%; font-size:9.5pt; margin:6px 0 10px; }
th, td { border:1px solid #bbb; padding:3px 6px; text-align:left; vertical-align:top; }
th { background:#f0f0f0; }
code { font-family: Consolas, monospace; font-size: 9.5pt; }
hr { border: none; border-top: 1px solid #ddd; margin: 10px 0; }
"""


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("--title", default="")
    args = p.parse_args()
    text = Path(args.src).read_text(encoding="utf-8")
    title = args.title or Path(args.src).stem
    body = convert(text)
    doc = (
        "<!DOCTYPE html><html lang=zh-CN><head><meta charset=utf-8>"
        f"<title>{html.escape(title)}</title><style>{CSS}</style></head>"
        f"<body>{body}</body></html>"
    )
    dst = Path(args.dst)
    dst.write_text(doc, encoding="utf-8")
    print(f"wrote {dst} ({dst.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

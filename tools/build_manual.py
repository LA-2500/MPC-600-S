#!/usr/bin/env python3
"""Build the MPC 600 S manual: docs/manual/manual.html with each {{shot:name|caption}} replaced
by that screen (shots/name.ppm, a P6 dump of the LCD) embedded as a PNG, written as one file.

usage: build_manual.py SHOTS_DIR OUT.html [OUT.pdf]

With OUT.pdf it also prints the PDF with Microsoft Edge (headless), in two passes: the first
finds the page each chapter starts on, the second prints with those numbers in the contents.
That needs pypdfium2, e.g.:
  uv run --no-project --with pypdfium2 python -I tools/build_manual.py SHOTS OUT.html OUT.pdf
"""
import base64, html, re, struct, subprocess, sys, zlib
from pathlib import Path

SCALE = 2  # each LCD pixel as 2x2: sharp at the printed size
EDGE = "/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"


def ppm_to_png(data: bytes) -> bytes:
    tok = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not tok:
        raise ValueError("not a P6 ppm")
    w, h = int(tok[1]), int(tok[2])
    px = data[tok.end():tok.end() + w * h * 3]
    rows = []
    for y in range(h):
        row = px[y * w * 3:(y + 1) * w * 3]
        wide = b"".join(row[x * 3:x * 3 + 3] * SCALE for x in range(w))
        rows += [b"\0" + wide] * SCALE
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w * SCALE, h * SCALE, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")


def build(shots: Path, pages: dict) -> str:
    src = (Path(__file__).resolve().parent.parent / "docs/manual/manual.html").read_text(encoding="utf-8")

    def figure(m):
        name, caption = m[1], m[2] or ""
        png = base64.b64encode(ppm_to_png((shots / f"{name}.ppm").read_bytes())).decode()
        cap = f"<figcaption>{caption}</figcaption>" if caption else ""
        return f'<figure><img src="data:image/png;base64,{png}" alt="{html.escape(caption or name)}">{cap}</figure>'

    text = re.sub(r"\{\{shot:([a-z0-9_]+)(?:\|([^}]*))?\}\}", figure, src)
    # The contents: each chapter's page, once known.
    def toc(m):
        page = pages.get(m[1])
        return m[0] + (f'<span class="pg">{page}</span>' if page else "")
    return re.sub(r'<a href="#(\w+)">[^<]*</a><span class="dots"></span>', toc, text)


def windows_path(p: Path) -> str:
    return subprocess.run(["wslpath", "-w", str(p)], capture_output=True, text=True, check=True).stdout.strip()


def print_pdf(html_path: Path, pdf_path: Path):
    url = "file:///" + windows_path(html_path).replace("\\", "/")
    subprocess.run([EDGE, "--headless", "--disable-gpu", "--no-pdf-header-footer",
                    f"--print-to-pdf={windows_path(pdf_path)}", url], capture_output=True, timeout=180, check=True)


def chapter_pages(src: str, pdf_path: Path) -> dict:
    """Each chapter id's page: the first page after the contents whose first lines are its label."""
    import pypdfium2
    labels = {m[1]: m[2].strip() for m in re.finditer(r'<h1 class="chapter" id="(\w+)"><small>([^<]+)</small>', src)}
    doc = pypdfium2.PdfDocument(str(pdf_path))
    texts = [doc[i].get_textpage().get_text_range() for i in range(len(doc))]
    pages = {}
    for cid, label in labels.items():
        for i, t in enumerate(texts):
            if i < 2:
                continue  # the title page and the contents
            head = [line.strip().upper() for line in t.splitlines()[:3]]
            if label.upper() in head:
                pages[cid] = i + 1
                break
    return pages


def main():
    shots, out = Path(sys.argv[1]), Path(sys.argv[2])
    pdf = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    text = build(shots, {})
    out.write_text(text, encoding="utf-8")
    if pdf:
        print_pdf(out, pdf)
        pages = chapter_pages(text, pdf)
        text = build(shots, pages)
        out.write_text(text, encoding="utf-8")
        print_pdf(out, pdf)
        print(f"{pdf}: chapters on pages {pages}")
    print(f"{out} ({len(text) // 1024} KB)")


if __name__ == "__main__":
    main()

# `md2pdf`: Markdown → PDF

This repo includes `tools/md2pdf.sh`, a small helper that converts any Markdown file into a nicer-looking PDF using `pandoc` + LaTeX.

## What it does

- Uses a wider text area (reduced margins) so the PDF isn’t “all border”.
- Uses nicer fonts (not the default Computer Modern look).
- Adds a running header + footer with a horizontal rule:
	- Header left: folder (repo-relative when possible)
	- Header right: file name
	- Footer left: date
	- Footer right: page `X/Y`

## Requirements (local)

- `pandoc`
- A TeX engine (preferred): `lualatex` (fallbacks: `xelatex`, then `pdflatex`)

## Usage

From the repo root:

```bash
# input.md -> <repo-root>/<input>.pdf (default)
tools/md2pdf.sh docs/Project-Evolution.md

# explicit output name
tools/md2pdf.sh Comment_007.md Comment_007.pdf
```

This is intended for local/offline generation. You generally should not commit generated PDFs unless you explicitly want them tracked.

## Options (environment variables)

You can tweak output without editing the script:

```bash
# Choose fonts
MAIN_FONT='Noto Serif' \
SANS_FONT='TeX Gyre Heros' \
MONO_FONT='DejaVu Sans Mono' \
tools/md2pdf.sh GHCP_COMMENTS.md

# Page setup
PAPER=a4 FONT_SIZE=12pt \
tools/md2pdf.sh README.md

# Margins (bigger text area = smaller margins)
GEOMETRY='left=1.2cm,right=1.2cm,top=1.4cm,bottom=1.6cm,includeheadfoot' \
tools/md2pdf.sh docs/README.md
```

Supported variables:

- `PDF_ENGINE`: `lualatex` | `xelatex` | `pdflatex` (default: auto)
- `MAIN_FONT`, `SANS_FONT`, `MONO_FONT`
- `GEOMETRY`: default is `left=1.5cm,right=1.5cm,top=1.6cm,bottom=1.8cm,includeheadfoot`
- `PAPER`: default `letter`
- `FONT_SIZE`: default `11pt`
- `NO_IMAGES`: set to `1` to omit embedded images (replaces them with filename references)

## Syntax highlighting (important)

By default, the script disables pandoc syntax highlighting.

Reason: pandoc’s default highlighting emits LaTeX macros inside code blocks, which can break when your code contains backslashes like `\r` / `\n` (common in embedded UART examples).

If you *want* highlighting anyway:

```bash
HIGHLIGHT=1 tools/md2pdf.sh GHCP_COMMENTS.md
```

## Debugging

If you hit a LaTeX error and want to inspect the generated header include:

```bash
KEEP_TMP=1 tools/md2pdf.sh some.md
```

That keeps the temporary header file (path is printed to stderr).

## Example: generate this doc’s PDF (local)

```bash
tools/md2pdf.sh tools/md2pdf.md tools/md2pdf.pdf

# print-friendly history page (omit pictures)
NO_IMAGES=1 tools/md2pdf.sh docs/Project-Evolution.md /tmp/Project-Evolution.noimg.pdf
```

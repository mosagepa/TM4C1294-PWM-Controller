#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/md2pdf.sh <input.md> [output.pdf]

Generates a nicer PDF via pandoc + LaTeX with:
- Wider text area (reduced margins)
- Nicer fonts (not Computer Modern)
- Running header/footer with a horizontal rule (file, folder, date, page)

Optional env vars:
  PDF_ENGINE   LaTeX engine: lualatex|xelatex|pdflatex (default: auto)
  MAIN_FONT    Main serif font (default: TeX Gyre Pagella)
  SANS_FONT    Sans font (default: TeX Gyre Heros)
  MONO_FONT    Monospace font (default: DejaVu Sans Mono)
  GEOMETRY     Pandoc geometry string (default: left=1.5cm,right=1.5cm,top=1.6cm,bottom=1.8cm,includeheadfoot)
  PAPER        Papersize (default: letter)
  FONT_SIZE    Document font size (default: 11pt)
  HIGHLIGHT    Set to 1 to enable pandoc syntax highlighting (default: 0)
  NO_IMAGES    Set to 1 to omit images (replace with filename reference)
  KEEP_TMP     Set to 1 to keep the generated LaTeX header include
EOF
}

if [[ ${1:-} == "-h" || ${1:-} == "--help" || $# -lt 1 ]]; then
  usage
  exit 0
fi

in_md="$1"
if [[ ! -f "$in_md" ]]; then
  echo "ERROR: input not found: $in_md" >&2
  exit 2
fi

# Default output: repo root (project folder) if inside a git repo; otherwise current directory.
in_base="$(basename "$in_md")"
in_stem="${in_base%.*}"
default_out_dir="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
out_pdf="${2:-$default_out_dir/${in_stem}.pdf}"

if ! command -v pandoc >/dev/null 2>&1; then
  echo "ERROR: pandoc not found in PATH" >&2
  exit 2
fi

pick_engine() {
  local requested="${PDF_ENGINE:-auto}"
  if [[ "$requested" != "auto" ]]; then
    echo "$requested"
    return
  fi
  if command -v lualatex >/dev/null 2>&1; then
    echo "lualatex"
  elif command -v xelatex >/dev/null 2>&1; then
    echo "xelatex"
  elif command -v pdflatex >/dev/null 2>&1; then
    echo "pdflatex"
  else
    echo ""
  fi
}

engine="$(pick_engine)"
if [[ -z "$engine" ]]; then
  echo "ERROR: no LaTeX engine found (need lualatex/xelatex/pdflatex)" >&2
  exit 2
fi

main_font="${MAIN_FONT:-TeX Gyre Pagella}"
sans_font="${SANS_FONT:-TeX Gyre Heros}"
mono_font="${MONO_FONT:-DejaVu Sans Mono}"
geometry="${GEOMETRY:-left=1.5cm,right=1.5cm,top=1.6cm,bottom=1.8cm,includeheadfoot}"
paper="${PAPER:-letter}"
font_size="${FONT_SIZE:-11pt}"

abs_in="$(readlink -f "$in_md")"
abs_dir="$(dirname "$abs_in")"
base_name="$(basename "$abs_in")"

repo_root="$(git -C "$abs_dir" rev-parse --show-toplevel 2>/dev/null || true)"
display_dir="$abs_dir"
display_file="$base_name"
if [[ -n "$repo_root" && "$abs_in" == "$repo_root"/* ]]; then
  rel_path="${abs_in#"$repo_root"/}"
  display_dir="$(dirname "$rel_path")"
  display_file="$(basename "$rel_path")"
fi

tmp_header="$(mktemp --tmpdir md2pdf_header_XXXXXX.tex)"
tmp_md=""

cleanup() {
  if [[ "${KEEP_TMP:-0}" != "1" ]]; then
    rm -f "$tmp_header" 2>/dev/null || true
  else
    echo "NOTE: KEEP_TMP=1 (kept temp header: $tmp_header)" >&2
  fi
  if [[ -n "${tmp_md:-}" ]]; then
    rm -f "$tmp_md" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# Escape a string for safe use in LaTeX (minimal set for file paths).
latex_escape() {
  python3 - <<'PY' "$1"
import sys

s = sys.argv[1]
repl = {
    "\\": r"\textbackslash{}",
    "&": r"\&",
    "%": r"\%",
    "#": r"\#",
    "_": r"\_",
    "{": r"\{",
    "}": r"\}",
    "$": r"\$",
    "^": r"\textasciicircum{}",
    "~": r"\textasciitilde{}",
}

print("".join(repl.get(ch, ch) for ch in s))
PY
}

safe_file="$(latex_escape "$display_file")"
safe_dir="$(latex_escape "$display_dir")"
build_date="$(date +%Y-%m-%d)"

cat >"$tmp_header" <<EOF
% md2pdf header/footer styling
\usepackage{fancyhdr}
\usepackage{lastpage}
\usepackage{titlesec}
\usepackage{hyperref}

\setlength{\headheight}{14pt}

\pagestyle{fancy}
\fancyhf{}
\renewcommand{\headrulewidth}{0.4pt}
\renewcommand{\footrulewidth}{0pt}

% Header: folder (L) | file (R)
\fancyhead[L]{\small\ttfamily ${safe_dir}}
\fancyhead[R]{\small\ttfamily ${safe_file}}

% Footer: date (L) | page X/Y (R)
\fancyfoot[L]{\small ${build_date}}
\fancyfoot[R]{\small Page \thepage/\pageref{LastPage}}

% A touch more breathing room for headings
\titleformat{\section}{\Large\bfseries}{\thesection}{0.6em}{}
\titleformat{\subsection}{\large\bfseries}{\thesubsection}{0.6em}{}
EOF

pandoc_args=(
  "$in_md"
  -f markdown-yaml_metadata_block
  -o "$out_pdf"
  --pdf-engine="$engine"
  -V "papersize=$paper"
  -V "fontsize=$font_size"
  -V "geometry:$geometry"
  --include-in-header="$tmp_header"
)

# Optional: omit images (replace embeds with filename references).
if [[ "${NO_IMAGES:-0}" == "1" ]]; then
  tmp_md="$(mktemp --tmpdir md2pdf_noimg_XXXXXX.md)"
  python3 - "$in_md" "$tmp_md" <<'PY'
import os
import re
import sys

src = sys.argv[1]
dst = sys.argv[2]

# Inline Markdown image: ![alt](path "title")
re_inline = re.compile(r"!\[[^\]]*\]\(([^\)\s]+)(?:\s+\"[^\"]*\")?\)")

# Very small HTML img handler: <img ... src="..." ...>
re_img = re.compile(r"<img[^>]*\ssrc=\"([^\"]+)\"[^>]*>", re.IGNORECASE)

def repl_path(path: str) -> str:
    # Keep both filename and a helpful path pointer.
    fname = os.path.basename(path)
    return f"[Image omitted: {fname} ({path})]"

with open(src, "r", encoding="utf-8") as f_in, open(dst, "w", encoding="utf-8") as f_out:
    for line in f_in:
        line = re_img.sub(lambda m: repl_path(m.group(1)), line)
        line = re_inline.sub(lambda m: repl_path(m.group(1)), line)
        f_out.write(line)
PY
  pandoc_args[0]="$tmp_md"
fi

# Pandoc's default syntax highlighting uses LaTeX macros inside code blocks.
# This breaks when your code contains backslashes like "\r" (they get treated as TeX commands).
# Default to no-highlight for robustness; allow opt-in.
if [[ "${HIGHLIGHT:-0}" != "1" ]]; then
  pandoc_args+=(--no-highlight)
fi

# Font selection: for Unicode engines use fontspec; for pdflatex use non-default serif.
if [[ "$engine" == "lualatex" || "$engine" == "xelatex" ]]; then
  pandoc_args+=(
    -V "mainfont=$main_font"
    -V "sansfont=$sans_font"
    -V "monofont=$mono_font"
    -V "linkcolor=blue"
    -V "urlcolor=blue"
  )
else
  # pdflatex fallback: choose a nicer text font than Computer Modern.
  # (newpx is usually present in TeXLive; if not, it will still compile without it.)
  printf '%s\n' "\\usepackage{newpxtext}" "\\usepackage{newpxmath}" >>"$tmp_header"
fi

pandoc "${pandoc_args[@]}"

# Confirm output
if [[ ! -f "$out_pdf" ]]; then
  echo "ERROR: failed to create: $out_pdf" >&2
  exit 2
fi

echo "OK: wrote $out_pdf"

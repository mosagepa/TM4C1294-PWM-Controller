#!/usr/bin/env bash
set -euo pipefail

start_dir="$(pwd)"
trap 'cd "$start_dir" >/dev/null 2>&1 || true' EXIT

usage() {
  cat <<'EOF'
Usage: backup_upload_gdrive.sh [PROJECT_DIR]

Creates a timestamped zip of the project folder (as a top-level folder in the zip),
uploads it as a single file to Google Drive using rclone, and verifies it exists.

Environment variables:
  RCLONE           rclone binary (default: rclone)
  RCLONE_REMOTE    rclone remote name (default: gdrive)
  GDRIVE_DIR       optional destination folder under Drive root (default: root)
  DRY_RUN          if set to 1, only prints actions (default: 0)

Examples:
  RCLONE_REMOTE=gdrive ./tools/backup_upload_gdrive.sh
  GDRIVE_DIR=Backups DRY_RUN=1 ./tools/backup_upload_gdrive.sh
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

rclone_bin="${RCLONE:-rclone}"
remote_name="${RCLONE_REMOTE:-gdrive}"
gdrive_dir="${GDRIVE_DIR:-}"
dry_run="${DRY_RUN:-0}"

project_dir="${1:-$start_dir}"
project_dir="$(realpath "$project_dir")"

if [[ ! -d "$project_dir" ]]; then
  echo "ERROR: PROJECT_DIR is not a directory: $project_dir" >&2
  exit 2
fi

project_base="$(basename "$project_dir")"
parent_dir="$(dirname "$project_dir")"
timestamp="$(date +%Y%m%d_%H%M%S)"
zip_name="${project_base}_${timestamp}.zip"
zip_path="${parent_dir}/${zip_name}"

if [[ -n "$gdrive_dir" ]]; then
  # Ensure trailing slash for directory target
  remote_path="${remote_name}:${gdrive_dir%/}/${zip_name}"
else
  remote_path="${remote_name}:${zip_name}"
fi

say() { printf '%s\n' "$*"; }
run() {
  if [[ "$dry_run" == "1" ]]; then
    say "+ $*"
  else
    "$@"
  fi
}

say "Project dir : $project_dir"
say "Zip output  : $zip_path"
say "Drive target: $remote_path"

# Build zip from parent so the archive contains the folder name at top-level.
cd "$parent_dir"

if command -v zip >/dev/null 2>&1; then
  run zip -r "$zip_name" "$project_base"
else
  say "ERROR: 'zip' not found. Install it (e.g., sudo apt-get install zip)." >&2
  exit 2
fi

# Upload single file (no sync semantics).
run "$rclone_bin" copyto "$zip_path" "$remote_path" --progress

# Verify file exists at destination.
if [[ "$dry_run" == "1" ]]; then
  say "DRY_RUN=1: skipping verification"
  exit 0
fi

say "Verifying: checking uploaded file directly (no full listing)"

# `rclone lsl` on the exact object path is fast and avoids enumerating a whole folder.
# Output format: "<size> <modtime> <path>".
remote_line="$("$rclone_bin" lsl "$remote_path" 2>/dev/null || true)"
if [[ -z "$remote_line" ]]; then
  say "VERIFY: FAIL - did not find $zip_name on Drive" >&2
  exit 3
fi

local_size_bytes="$(stat -c%s "$zip_path" 2>/dev/null || echo "")"
remote_size_bytes="$(awk '{print $1}' <<<"$remote_line" 2>/dev/null || echo "")"

say "VERIFY: OK - found $zip_name on Drive"
if [[ -n "$local_size_bytes" && -n "$remote_size_bytes" && "$local_size_bytes" != "$remote_size_bytes" ]]; then
  say "VERIFY: WARNING - size mismatch (local=$local_size_bytes, remote=$remote_size_bytes)" >&2
  exit 4
fi

#!/usr/bin/env bash
set -euo pipefail

# Sync this repo to a remote Linux box (e.g. your lab machine), build, flash,
# run the scripted UART session test, and pull back the logs+report.
#
# This script NEVER stores passwords. If your SSH uses password auth,
# you'll be prompted by ssh/scp/rsync.
#
# Usage (from repo root or anywhere):
#   tools/remote_dijkstra_autotest.sh
#
# Common overrides (env vars):
#   DIJKSTRA_HOST=mosagepa@dijkstra
#   DIJKSTRA_DIR=~/decomp/.../integr_v03   # default mirrors local path under $HOME when possible
#   UART0_DEV=/dev/ttyACM0
#   UART3_DEV=/dev/ttyUSB0
#   DURATION=25
#   SCRIPT_FILE=tools/test_scripts/human_like_uart3.txt
#   WAIT_PROMPT=1            # 1=wait for prompt between sends, 0=do not
#   PROMPT_TIMEOUT=8.0       # seconds
#   SEND_DELAY=0.6           # seconds before first send
#   TYPE_DELAY=0.02          # seconds between bytes for 'type' steps
#   SUDO_CMD=sudo           # set to empty for sudo-less flashing: SUDO_CMD=
#   NO_FLASH=1              # skip flashing
#   LOCAL_BUILD=1            # default: build locally, do not build on remote
#   PYENV_VERSION=3.12.6     # used if pyenv is available on remote
#   PYENV_MODE=exec           # exec (default) or shell
#   REMOTE_PYTHON=python3    # override remote python command if needed
#   SSH_CMD=ssh  RSYNC_CMD=rsync  SCP_CMD=scp

HOST="${DIJKSTRA_HOST:-mosagepa@dijkstra}"
UART0_DEV="${UART0_DEV:-/dev/ttyACM0}"
UART3_DEV="${UART3_DEV:-/dev/ttyUSB0}"
DURATION="${DURATION:-25}"
SCRIPT_FILE="${SCRIPT_FILE:-tools/test_scripts/human_like_uart3.txt}"
WAIT_PROMPT="${WAIT_PROMPT:-1}"
PROMPT_TIMEOUT="${PROMPT_TIMEOUT:-8.0}"
SEND_DELAY="${SEND_DELAY:-0.6}"
TYPE_DELAY="${TYPE_DELAY:-0.02}"
SUDO_CMD="${SUDO_CMD:-sudo}"
NO_FLASH="${NO_FLASH:-0}"
LOCAL_BUILD="${LOCAL_BUILD:-1}"
PYENV_VERSION="${PYENV_VERSION:-3.12.6}"
PYENV_MODE="${PYENV_MODE:-exec}"
REMOTE_PYTHON="${REMOTE_PYTHON:-python3}"
QUIET="${QUIET:-1}"

SSH="${SSH_CMD:-ssh}"
RSYNC="${RSYNC_CMD:-rsync}"
SCP="${SCP_CMD:-scp}"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"

if [[ ! -f "${REPO_DIR}/Makefile" ]]; then
  echo "ERROR: expected repo root at ${REPO_DIR} (Makefile missing)" >&2
  exit 2
fi

# Default remote directory mirrors the local absolute path under $HOME, so the
# remote location looks like ~/decomp/.../integr_v03 without needing manual cd.
DEFAULT_REMOTE_DIR="~/integr_v03"
if [[ "${REPO_DIR}" == "${HOME}/"* ]]; then
  DEFAULT_REMOTE_DIR="~/${REPO_DIR#"${HOME}/"}"
fi
REMOTE_DIR="${DIJKSTRA_DIR:-${DEFAULT_REMOTE_DIR}}"

if [[ "${LOCAL_BUILD}" == "1" ]]; then
  echo "[1/4] Building locally..."
  (cd "${REPO_DIR}" && make -j)
fi

echo "[2/4] Syncing repo to ${HOST}:${REMOTE_DIR} ..."
# Exclude transient outputs and local logs; keep the firmware .bin for flashing.
"${RSYNC}" -az --delete \
  --exclude '.git' \
  --exclude 'logs' \
  --exclude '*.o' --exclude '*.d' --exclude '*.axf' --exclude '*.lst' \
  "${REPO_DIR}/" "${HOST}:${REMOTE_DIR}/"

REMOTE_CMD=(
  "set -euo pipefail"
  "cd ${REMOTE_DIR}"
  # Best-effort: ensure the non-interactive ssh session sees the same PATH/pyenv
  # setup as an interactive terminal.
  "if [ -f ~/.bashrc ]; then source ~/.bashrc; fi"
  "if [ -f ~/.profile ]; then source ~/.profile; fi"
  "if [ -f ~/.bash_profile ]; then source ~/.bash_profile; fi"

  # Use pyenv python if available; otherwise fall back to system python3.
  "PYENV_BIN=''"
  "if command -v pyenv >/dev/null 2>&1; then PYENV_BIN='pyenv'; fi"
  "if [ -z \"\$PYENV_BIN\" ] && [ -x \"\$HOME/.pyenv/bin/pyenv\" ]; then export PATH=\"\$HOME/.pyenv/bin:\$PATH\"; PYENV_BIN='pyenv'; fi"
  "USE_PYENV=0"
  "if [ -n \"\$PYENV_BIN\" ]; then USE_PYENV=1; fi"

  "PYENV_SHELL_OK=0"
  "if [ \"\$USE_PYENV\" = '1' ] && [ '${PYENV_MODE}' = 'shell' ]; then eval \"\$(pyenv init -)\" || true; if pyenv shell '${PYENV_VERSION}' >/dev/null 2>&1; then PYENV_SHELL_OK=1; fi; fi"

  "echo '[remote] python:'"
  "if [ \"\$PYENV_SHELL_OK\" = '1' ]; then python3 -c 'import sys; print(sys.version)'; elif [ \"\$USE_PYENV\" = '1' ]; then PYENV_VERSION='${PYENV_VERSION}' \$PYENV_BIN exec python3 -c 'import sys; print(sys.version)'; else ${REMOTE_PYTHON} -c 'import sys; print(sys.version)'; fi"
)

if [[ "${NO_FLASH}" != "1" ]]; then
  echo "[3/4] Flashing + running UART automation on remote..."
  # We intentionally avoid invoking remote `make` because the remote host may
  # not have arm-none-eabi-gcc or the TivaWare driverlib tree. Flash the .bin
  # directly.
  REMOTE_CMD+=(
    "command -v lm4flash >/dev/null 2>&1 || { echo 'ERROR: lm4flash not found on remote (install lm4tools)'; exit 2; }"
    "test -f integr_V03.bin || { echo 'ERROR: integr_V03.bin missing (did local build run?)'; ls -la; exit 2; }"
    "${SUDO_CMD} lm4flash integr_V03.bin"
  )
else
  echo "[3/4] Skipping flash (NO_FLASH=1); running UART automation on remote..."
fi

REMOTE_CMD+=(
  "RUN_ID='${RUN_ID}'"
  "REMOTE_LOG_DIR='logs_remote/${RUN_ID}'"
  "mkdir -p \"\$REMOTE_LOG_DIR\""

  # Host-side diagnostics: helps distinguish firmware issues vs USB/host issues.
  "{ echo '=== host pre ==='; date; uname -a; } > \"\$REMOTE_LOG_DIR/host_pre.txt\" 2>&1 || true"
  "{ echo '=== /dev pre ==='; ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null; ls -l /dev/serial/by-id/* 2>/dev/null; } > \"\$REMOTE_LOG_DIR/dev_pre.txt\" 2>&1 || true"
  "{ echo '=== dmesg tail pre ==='; dmesg | tail -n 120; } > \"\$REMOTE_LOG_DIR/dmesg_pre.txt\" 2>&1 || true"
  "{ echo '=== journalctl -k tail pre ==='; journalctl -k -n 120 --no-pager 2>/dev/null; } > \"\$REMOTE_LOG_DIR/journal_k_pre.txt\" 2>&1 || true"

  "UART0='${UART0_DEV}'"
  "UART3='${UART3_DEV}'"
  "if [ -d /dev/serial/by-id ]; then if [ \"\$UART0\" = '/dev/ttyACM0' ]; then UART0_BYID=\"\$(ls -1 /dev/serial/by-id/*In-Circuit_Debug_Interface* 2>/dev/null | head -n 1)\"; if [ -n \"\$UART0_BYID\" ]; then UART0=\"\$UART0_BYID\"; fi; fi; if [ \"\$UART3\" = '/dev/ttyUSB0' ]; then UART3_BYID=\"\$(ls -1 /dev/serial/by-id/*FTDI*UART* /dev/serial/by-id/*FT232* 2>/dev/null | head -n 1)\"; if [ -n \"\$UART3_BYID\" ]; then UART3=\"\$UART3_BYID\"; fi; fi; fi"
  "echo \"[remote] uart0=\$UART0 uart3=\$UART3\""
  "ls -l \"\$UART0\" \"\$UART3\" 2>/dev/null || true"
  "ls -l /dev/ttyUSB* 2>/dev/null || true"
  "ls -l /dev/serial/by-id/* 2>/dev/null || true"
  "test -e \"\$UART3\" || { echo 'ERROR: UART3 device not found on remote.'; echo 'Hint: set UART3_DEV=/dev/ttyUSB1 (or /dev/serial/by-id/...)'; exit 2; }"
  "echo '[remote] checking pyserial (should be instant)...'"
  "if [ \"\$PYENV_SHELL_OK\" = '1' ]; then python3 -c \"import serial; print('REMOTE: pyserial ok')\" || PYENV_SHELL_OK=0; fi"
  "if [ \"\$PYENV_SHELL_OK\" != '1' ]; then if [ \"\$USE_PYENV\" = '1' ]; then PYENV_VERSION='${PYENV_VERSION}' \$PYENV_BIN exec python3 -c \"import serial; print('REMOTE: pyserial ok')\"; else ${REMOTE_PYTHON} -c \"import serial; print('REMOTE: pyserial ok')\"; fi; fi"
  "if [ '${QUIET}' = '1' ]; then QFLAG='--quiet'; else QFLAG=''; fi"
  "echo '[remote] starting uart_session.py (this is the slow part; duration=${DURATION}s)...'"
  # Note: the Jan-13 snapshot uart_session.py does not support --analyze/--report.
  "ARGS=(tools/uart_session.py --uart0 \"\$UART0\" --uart3 \"\$UART3\" --uart3-script-file \"${SCRIPT_FILE}\" --duration ${DURATION} --send-delay ${SEND_DELAY} --type-delay ${TYPE_DELAY} --logs-dir \"\$REMOTE_LOG_DIR\")"
  "if [ '${WAIT_PROMPT}' = '1' ]; then ARGS+=(--wait-prompt --prompt-timeout ${PROMPT_TIMEOUT}); fi"
  "if [ -n \"\$QFLAG\" ]; then ARGS+=(\$QFLAG); fi"
  "if [ \"\$PYENV_SHELL_OK\" = '1' ]; then python3 \"\${ARGS[@]}\"; elif [ \"\$USE_PYENV\" = '1' ]; then PYENV_VERSION='${PYENV_VERSION}' \$PYENV_BIN exec python3 \"\${ARGS[@]}\"; else ${REMOTE_PYTHON} \"\${ARGS[@]}\"; fi"
  "{ echo '=== /dev post ==='; ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null; ls -l /dev/serial/by-id/* 2>/dev/null; } > \"\$REMOTE_LOG_DIR/dev_post.txt\" 2>&1 || true"
  "{ echo '=== dmesg tail post ==='; dmesg | tail -n 120; } > \"\$REMOTE_LOG_DIR/dmesg_post.txt\" 2>&1 || true"
  "{ echo '=== journalctl -k tail post ==='; journalctl -k -n 120 --no-pager 2>/dev/null; } > \"\$REMOTE_LOG_DIR/journal_k_post.txt\" 2>&1 || true"
  "echo '[remote] done'"
)

# Important: run through a login shell so PATH/pyenv/toolchain setup from the
# remote user's profile is applied. This avoids surprises where interactive
# commands work on the remote terminal but fail under non-interactive ssh.
REMOTE_CHAINED="$(printf '%s && ' "${REMOTE_CMD[@]}") true"

set +e
"${SSH}" "${HOST}" "bash -lc $(printf %q "${REMOTE_CHAINED}")"
REMOTE_RC=$?
set -e

echo "[4/4] Pulling back reports/logs..."
LOCAL_OUT_DIR="${REPO_DIR}/logs_dijkstra/${RUN_ID}"
mkdir -p "${LOCAL_OUT_DIR}"
# Pull only this run's files; avoids copying the entire historical logs set.
set +e
"${SCP}" "${HOST}:${REMOTE_DIR}/logs_remote/${RUN_ID}/*" "${LOCAL_OUT_DIR}/" 2>/dev/null
set -e

echo "Saved results under: ${LOCAL_OUT_DIR}"
ls -1 "${LOCAL_OUT_DIR}" | tail -n 50 || true

exit "${REMOTE_RC}"

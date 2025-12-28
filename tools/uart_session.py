#!/usr/bin/env python3
"""UART capture + optional command send for TM4C1294-PWM-Controller.

- Captures UART0 (ICDI) @9600 by default
- Captures UART3 (USER) @115200 by default
- Writes timestamped logs under ./logs/
- Optionally sends a command to UART3 (e.g. "PSYN 44\r")

Requires: pyserial (already present if you use `python3 -m serial.tools.miniterm`).
"""

from __future__ import annotations

import argparse
import os
import queue
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

try:
    import serial  # type: ignore
except Exception as exc:  # pragma: no cover
    print("ERROR: pyserial is required. Try: pip3 install pyserial", file=sys.stderr)
    raise


@dataclass
class PortSpec:
    name: str
    device: str
    baud: int


class _PromptWatch:
    def __init__(self, needle: bytes) -> None:
        self._needle = needle
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._event = threading.Event()
        self._count = 0

    def feed(self, data: bytes) -> None:
        if not data:
            return

        with self._lock:
            # Keep a small rolling window; prompts are short.
            self._buf.extend(data)
            if len(self._buf) > 4096:
                del self._buf[:-2048]

            if self._needle in self._buf:
                self._count += 1
                self._event.set()

    def count(self) -> int:
        with self._lock:
            return self._count

    def wait_for_next(self, last_count: int, timeout_s: float) -> bool:
        deadline = time.monotonic() + max(0.0, timeout_s)
        while True:
            if self.count() > last_count:
                return True
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            self._event.wait(timeout=min(0.25, remaining))
            self._event.clear()


def _ts() -> str:
    # ISO-ish but filename-safe
    return time.strftime("%Y%m%d_%H%M%S", time.localtime())


def _mkdir_logs(base_dir: Path) -> Path:
    base_dir.mkdir(parents=True, exist_ok=True)
    return base_dir


def _reader_thread(
    spec: PortSpec,
    out_q: "queue.Queue[tuple[str, bytes]]",
    err_q: "queue.Queue[str]",
    stop: threading.Event,
) -> None:
    try:
        ser = serial.Serial(spec.device, spec.baud, timeout=0.2)
    except Exception as exc:
        msg = f"ERROR opening {spec.device} @ {spec.baud}: {exc}"
        err_q.put(msg)
        out_q.put((spec.name, f"[{msg}]\n".encode("utf-8", "replace")))
        return

    with ser:
        while not stop.is_set():
            try:
                chunk = ser.read(4096)
            except Exception as exc:
                msg = f"ERROR read {spec.device}: {exc}"
                err_q.put(msg)
                out_q.put((spec.name, f"[{msg}]\n".encode("utf-8", "replace")))
                return

            if chunk:
                out_q.put((spec.name, chunk))


def _open_for_send(device: str, baud: int, dtr: Optional[bool] = None) -> "serial.Serial":
    ser = serial.Serial(device, baud, timeout=1)
    if dtr is not None:
        try:
            ser.dtr = bool(dtr)
        except Exception:
            pass
    return ser


def _preflight_port(device: str, baud: int, *, name: str, pulse_dtr: bool = False) -> Optional[str]:
    """Best-effort: open UART, flush buffers, optionally pulse DTR, close.

    This helps ensure the OS driver and device are in a clean state before we
    start capture/sends.

    Returns an error string on failure, else None.
    """
    try:
        ser = serial.Serial(device, baud, timeout=0.2)
    except Exception as exc:
        return f"preflight: ERROR opening {name} {device} @ {baud}: {exc}"

    try:
        # Clear any stale host-side buffers.
        try:
            ser.reset_input_buffer()
        except Exception:
            pass
        try:
            ser.reset_output_buffer()
        except Exception:
            pass

        # Optional DTR pulse for targets that use DTR as a session boundary.
        if pulse_dtr:
            try:
                ser.dtr = False
                time.sleep(0.05)
                ser.dtr = True
                time.sleep(0.05)
            except Exception:
                pass

        # Drain any immediately-available bytes.
        try:
            _ = ser.read(4096)
        except Exception:
            pass
    finally:
        try:
            ser.close()
        except Exception:
            pass

    return None


def _decode_escapes(text: str) -> bytes:
    # Supports \r and \n escapes in a shell-friendly way.
    return text.encode("utf-8").decode("unicode_escape").encode("latin1", "replace")


def _ensure_trailing_crlf(payload: bytes) -> bytes:
    """Ensure the payload ends with CRLF (\r\n).

    This matches typical terminal ENTER behavior and avoids sending only CR or only LF.
    """
    if not payload:
        return b"\r\n"

    if payload.endswith(b"\r\n"):
        return payload
    if payload.endswith(b"\r"):
        return payload + b"\n"
    if payload.endswith(b"\n"):
        # Convert trailing LF to CRLF.
        if len(payload) >= 2 and payload[-2:] == b"\r\n":
            return payload
        return payload[:-1] + b"\r\n"
    return payload + b"\r\n"


@dataclass
class ScriptStep:
    kind: str  # 'send' | 'type' | 'sleep' | 'dtr'
    value: object


def _parse_uart3_script(text: str) -> list[ScriptStep]:
    """Parse a tiny script language.

    Lines (case-insensitive), with optional comments after '#':
    - send <text-with-\\r-\\n-escapes>     (auto-CRLF unless --no-auto-crlf)
    - send                              (press ENTER: sends CRLF)
            - type <text-with-\\r-\\n-escapes>     (sends bytes as-is; no auto-CRLF)
      - sleep <seconds>
      - dtr <0|1>
    """
    steps: list[ScriptStep] = []
    for idx, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue

        parts = line.split(maxsplit=1)
        cmd = parts[0].lower()
        arg = parts[1].strip() if len(parts) > 1 else ""

        if cmd == "send":
            steps.append(ScriptStep("send", arg))
        elif cmd == "type":
            if not arg:
                raise ValueError(f"uart3-script line {idx}: missing payload for 'type'")
            steps.append(ScriptStep("type", arg))
        elif cmd == "sleep":
            if not arg:
                raise ValueError(f"uart3-script line {idx}: missing seconds for 'sleep'")
            steps.append(ScriptStep("sleep", float(arg)))
        elif cmd == "dtr":
            if arg not in {"0", "1"}:
                raise ValueError(f"uart3-script line {idx}: dtr must be 0 or 1")
            steps.append(ScriptStep("dtr", arg == "1"))
        else:
            raise ValueError(f"uart3-script line {idx}: unknown command '{cmd}'")

    return steps


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Capture UART(s) and optionally send a command")

    parser.add_argument("--uart0", default=os.environ.get("UART0_DEV", "/dev/ttyACM0"))
    parser.add_argument("--uart0-baud", type=int, default=int(os.environ.get("UART0_BAUD", "9600")))

    parser.add_argument("--uart3", default=os.environ.get("UART3_DEV", "/dev/ttyUSB1"))
    parser.add_argument("--uart3-baud", type=int, default=int(os.environ.get("UART3_BAUD", "115200")))

    parser.add_argument("--no-uart0", action="store_true", help="Disable UART0 capture")
    parser.add_argument("--no-uart3", action="store_true", help="Disable UART3 capture")

    parser.add_argument(
        "--send-uart3",
        action="append",
        default=[],
        help=r"Send this byte string to UART3 (supports \r and \n escapes). Can be repeated. Default behavior auto-appends CRLF. Example: --send-uart3 'PSYN 44'",
    )
    parser.add_argument(
        "--no-auto-crlf",
        action="store_true",
        help="Do not auto-append/normalize trailing CRLF on UART3 sends.",
    )
    parser.add_argument(
        "--uart3-dtr",
        choices=["0", "1"],
        default=None,
        help="Optionally set UART3 DTR when opening the send port (0/1).",
    )
    parser.add_argument(
        "--uart3-script",
        default=None,
        help="Inline UART3 script: lines of 'send ...', 'sleep ...', 'dtr 0|1'.",
    )
    parser.add_argument(
        "--uart3-script-file",
        default=None,
        help="Path to a UART3 script file (same syntax as --uart3-script).",
    )
    parser.add_argument(
        "--send-delay",
        type=float,
        default=0.6,
        help="Seconds to wait before sending on UART3 (lets target boot). Default: 0.6",
    )
    parser.add_argument(
        "--send-interval",
        type=float,
        default=0.20,
        help="Seconds to wait between repeated --send-uart3 payloads.",
    )

    parser.add_argument(
        "--preflight",
        action="store_true",
        help="(Deprecated) Preflight is ON by default; use --no-preflight to disable.",
    )
    parser.add_argument(
        "--no-preflight",
        action="store_true",
        help="Disable UART preflight (default is ON).",
    )
    parser.add_argument(
        "--preflight-pulse-dtr",
        action="store_true",
        help="During preflight, briefly pulse DTR on UART3 (best-effort).",
    )

    parser.add_argument(
        "--postflight",
        action="store_true",
        help="(Deprecated) Postflight is ON by default; use --no-postflight to disable.",
    )
    parser.add_argument(
        "--no-postflight",
        action="store_true",
        help="Disable UART postflight cleanup (default is ON).",
    )
    parser.add_argument(
        "--postflight-drop-dtr",
        action="store_true",
        help="During postflight, set UART3 DTR low before closing (best-effort).",
    )

    parser.add_argument(
        "--wait-prompt",
        action="store_true",
        help="When sending multiple UART3 commands, wait for the next prompt (> ) before sending the next one.",
    )
    parser.add_argument(
        "--prompt-bytes",
        default="> ",
        help="Prompt marker to wait for (default: '> ').",
    )
    parser.add_argument(
        "--prompt-timeout",
        type=float,
        default=8.0,
        help="Seconds to wait for the prompt between sends when --wait-prompt is used.",
    )

    parser.add_argument(
        "--type-delay",
        type=float,
        default=0.02,
        help="Seconds delay between bytes when UART3 script uses 'type' (default: 0.02).",
    )

    parser.add_argument("--duration", type=float, default=10.0, help="Seconds to capture before exiting (0 = run forever)")
    parser.add_argument("--logs-dir", default="logs", help="Directory to store capture logs")
    parser.add_argument("--quiet", action="store_true", help="Do not echo to stdout")

    args = parser.parse_args(argv)

    logs_dir = _mkdir_logs(Path(args.logs_dir))
    stamp = _ts()

    specs: list[PortSpec] = []
    if not args.no_uart0:
        specs.append(PortSpec("UART0", args.uart0, args.uart0_baud))
    if not args.no_uart3:
        specs.append(PortSpec("UART3", args.uart3, args.uart3_baud))

    if not specs:
        print("ERROR: nothing to do (both UARTs disabled)", file=sys.stderr)
        return 2

    out_q: "queue.Queue[tuple[str, bytes]]" = queue.Queue()
    err_q: "queue.Queue[str]" = queue.Queue()
    stop = threading.Event()

    # Track prompt appearance on UART3 to safely sequence multi-command sends.
    prompt_watch = _PromptWatch(args.prompt_bytes.encode("utf-8", "replace"))

    send_failed = False

    log_files: dict[str, Path] = {}
    log_fds: dict[str, object] = {}
    threads: list[threading.Thread] = []
    try:
        script_text: Optional[str] = None
        if args.uart3_script_file:
            try:
                script_text = Path(args.uart3_script_file).read_text(encoding="utf-8")
            except Exception as exc:
                send_failed = True
                out_q.put(("UART3", f"[ERROR reading script file]: {exc}\n".encode("utf-8", "replace")))
        elif args.uart3_script:
            script_text = args.uart3_script


        # Preflight/postflight are ON by default. Use --no-preflight/--no-postflight to disable.
        # (We keep --preflight/--postflight flags for compatibility, but they are redundant now.)
        do_preflight = not bool(args.no_preflight)
        do_postflight = not bool(args.no_postflight)

        if do_preflight:
            # Preflight before we start reader threads so we don't open the same
            # device twice at the same time.
            if not args.no_uart0:
                err = _preflight_port(args.uart0, args.uart0_baud, name="UART0", pulse_dtr=False)
                if err:
                    err_q.put(err)
                    out_q.put(("UART0", f"[{err}]\n".encode("utf-8", "replace")))
            if not args.no_uart3:
                err = _preflight_port(
                    args.uart3,
                    args.uart3_baud,
                    name="UART3",
                    pulse_dtr=bool(args.preflight_pulse_dtr),
                )
                if err:
                    err_q.put(err)
                    out_q.put(("UART3", f"[{err}]\n".encode("utf-8", "replace")))

        for spec in specs:
            path = logs_dir / f"{stamp}_{spec.name}_{Path(spec.device).name}_{spec.baud}.log"
            log_files[spec.name] = path
            log_fds[spec.name] = open(path, "ab", buffering=0)

        threads = [
            threading.Thread(target=_reader_thread, args=(spec, out_q, err_q, stop), daemon=True)
            for spec in specs
        ]
        for t in threads:
            t.start()

        # Only wait for prompt when explicitly requested.
        wait_prompt = bool(args.wait_prompt)
        auto_crlf = not bool(args.no_auto_crlf)

        if script_text is not None or args.send_uart3:
            time.sleep(max(0.0, args.send_delay))
            dtr_init: Optional[bool] = None
            if args.uart3_dtr is not None:
                dtr_init = args.uart3_dtr == "1"

            try:
                with _open_for_send(args.uart3, args.uart3_baud, dtr=dtr_init) as ser:
                    # Script mode (preferred when present)
                    if script_text is not None:
                        steps = _parse_uart3_script(script_text)
                        last_prompt = prompt_watch.count()
                        for step in steps:
                            if step.kind == "sleep":
                                time.sleep(max(0.0, float(step.value)))
                            elif step.kind == "dtr":
                                try:
                                    ser.dtr = bool(step.value)
                                except Exception:
                                    pass
                            elif step.kind == "send":
                                payload = _decode_escapes(str(step.value))
                                if auto_crlf:
                                    payload = _ensure_trailing_crlf(payload)
                                ser.write(payload)
                                ser.flush()
                                if wait_prompt:
                                    prompt_watch.wait_for_next(last_prompt, args.prompt_timeout)
                                    last_prompt = prompt_watch.count()
                            elif step.kind == "type":
                                payload = _decode_escapes(str(step.value))
                                # Send bytes as-is (no CRLF normalization), optionally throttled.
                                for b in payload:
                                    ser.write(bytes([b]))
                                    ser.flush()
                                    if args.type_delay > 0:
                                        time.sleep(args.type_delay)
                            else:
                                raise RuntimeError(f"Unknown script step: {step.kind}")
                    else:
                        # Simple multi-send mode
                        last_prompt = prompt_watch.count()
                        if wait_prompt:
                            # Wait for the initial prompt once so the first command
                            # isn't sent while the target is still booting.
                            prompt_watch.wait_for_next(last_prompt, args.prompt_timeout)
                            last_prompt = prompt_watch.count()
                        for i, txt in enumerate(args.send_uart3):
                            payload = _decode_escapes(txt)
                            if auto_crlf:
                                payload = _ensure_trailing_crlf(payload)
                            ser.write(payload)
                            ser.flush()
                            if wait_prompt:
                                prompt_watch.wait_for_next(last_prompt, args.prompt_timeout)
                                last_prompt = prompt_watch.count()
                            elif i + 1 < len(args.send_uart3):
                                time.sleep(max(0.0, args.send_interval))
            except Exception as exc:
                send_failed = True
                msg = f"[ERROR sending to {args.uart3} @ {args.uart3_baud}]: {exc}\n"
                out_q.put(("UART3", msg.encode("utf-8", "replace")))

        start = time.monotonic()
        while True:
            if args.duration > 0 and (time.monotonic() - start) >= args.duration:
                break

            try:
                name, data = out_q.get(timeout=0.2)
            except queue.Empty:
                continue

            prefix = f"[{name}] "
            ts_line = time.strftime("%H:%M:%S", time.localtime()).encode("ascii")
            # Keep raw bytes intact in files; prefix+timestamp are textual.
            line = b"".join([b"[", ts_line, b"] ", prefix.encode("utf-8"), data])

            # Feed raw UART3 data into the prompt watcher.
            if name == "UART3":
                prompt_watch.feed(data)

            fd = log_fds.get(name)
            if fd is not None:
                fd.write(line)

            if not args.quiet:
                try:
                    sys.stdout.buffer.write(line)
                    sys.stdout.buffer.flush()
                except Exception:
                    pass

    finally:
        stop.set()

        # Give reader threads time to exit and close their serial handles.
        for t in threads:
            try:
                t.join(timeout=1.0)
            except Exception:
                pass

        for fd in log_fds.values():
            try:
                fd.close()
            except Exception:
                pass

        # Postflight cleanup: open/flush/close to ensure ports are released.
        # This helps prevent the test run from leaving the UART devices in a weird state.
        try:
            if 'do_postflight' in locals() and do_postflight:
                if not args.no_uart0:
                    _preflight_port(args.uart0, args.uart0_baud, name="UART0", pulse_dtr=False)
                if not args.no_uart3:
                    if args.postflight_drop_dtr:
                        # Best-effort: explicitly drop DTR then close.
                        try:
                            s = serial.Serial(args.uart3, args.uart3_baud, timeout=0.2)
                            try:
                                s.dtr = False
                            except Exception:
                                pass
                            try:
                                s.reset_input_buffer()
                            except Exception:
                                pass
                            try:
                                s.reset_output_buffer()
                            except Exception:
                                pass
                            try:
                                s.close()
                            except Exception:
                                pass
                        except Exception:
                            pass
                    else:
                        _preflight_port(args.uart3, args.uart3_baud, name="UART3", pulse_dtr=False)
        except Exception:
            pass


    if not args.quiet:
        for name, path in log_files.items():
            print(f"\nSaved {name} log: {path}")

    had_uart_errors = not err_q.empty()
    ok = (not send_failed) and (not had_uart_errors)

    if not args.quiet:
        print(f"\nRESULT: {'OK' if ok else 'FAIL'}")
        if had_uart_errors:
            print("Errors:")
            while not err_q.empty():
                print(f"- {err_q.get()}")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

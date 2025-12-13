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
        default=None,
        help=r"Send this exact byte string to UART3 (supports \r and \n escapes). Example: 'PSYN 44\r'",
    )
    parser.add_argument(
        "--send-delay",
        type=float,
        default=0.25,
        help="Seconds to wait before sending on UART3 (lets target boot).",
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

    send_failed = False

    log_files: dict[str, Path] = {}
    log_fds: dict[str, object] = {}
    try:
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

        if args.send_uart3 is not None:
            time.sleep(max(0.0, args.send_delay))
            payload = args.send_uart3.encode("utf-8").decode("unicode_escape").encode("latin1", "replace")
            try:
                with _open_for_send(args.uart3, args.uart3_baud) as ser:
                    ser.write(payload)
                    ser.flush()
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
        for fd in log_fds.values():
            try:
                fd.close()
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

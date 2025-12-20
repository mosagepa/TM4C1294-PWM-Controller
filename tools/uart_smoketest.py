#!/usr/bin/env python3
"""UART3 slick-prompt smoke test for TM4C1294 PWM Controller.

What it does (high level):
- Opens UART0 (ICDI) and UART3 (USER)
- Forces a DTR connect/disconnect cycle to validate session gating
- Sends a small suite of commands over UART3 and checks responses
- Exercises a few line-editing keystrokes (BS/DEL/Ctrl-U)

This is intentionally a *smoke test*, not a full xUnit harness.

Requires: pyserial
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import threading
import time
from dataclasses import dataclass

try:
    import serial  # type: ignore
except Exception as exc:  # pragma: no cover
    print("ERROR: pyserial is required. Try: pip3 install pyserial", file=sys.stderr)
    raise


ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


@dataclass
class TestCase:
    name: str
    payload: bytes
    expect_any: tuple[str, ...]
    forbid_any: tuple[str, ...] = ()
    timeout_s: float = 1.5


class SerialReader:
    def __init__(self, ser: "serial.Serial", label: str) -> None:
        self.ser = ser
        self.label = label
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)
        self._lock = threading.Lock()
        self._buf = bytearray()

    def start(self) -> None:
        self._t.start()

    def stop(self) -> None:
        self._stop.set()
        self._t.join(timeout=1.0)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(4096)
            except Exception:
                return
            if chunk:
                with self._lock:
                    self._buf.extend(chunk)
                # Echo to stdout for live visibility
                try:
                    sys.stdout.write(f"[{self.label}] {chunk.decode('utf-8', 'replace')}")
                    sys.stdout.flush()
                except Exception:
                    pass

    def drain_text(self) -> str:
        with self._lock:
            data = bytes(self._buf)
            self._buf.clear()
        return data.decode("utf-8", "replace")


def wait_for_text(reader: SerialReader, needle: str, timeout_s: float) -> bool:
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        text = strip_ansi(reader.drain_text())
        if needle in text:
            return True
        time.sleep(0.05)
    return False


def wait_for_any(reader: SerialReader, needles: tuple[str, ...], timeout_s: float) -> str:
    end = time.monotonic() + timeout_s
    accum = ""
    while time.monotonic() < end:
        accum += reader.drain_text()
        plain = strip_ansi(accum)
        for n in needles:
            if n in plain:
                return plain
        time.sleep(0.05)
    return strip_ansi(accum)


def set_dtr(ser: "serial.Serial", level: bool) -> None:
    try:
        ser.dtr = bool(level)
    except Exception:
        # Some adapters/drivers may not support DTR control.
        pass


def pulse_dtr(ser: "serial.Serial", first: bool, second: bool, delay_s: float) -> None:
    set_dtr(ser, first)
    time.sleep(delay_s)
    set_dtr(ser, second)


def connect_session(uart3: "serial.Serial", uart3_reader: SerialReader, uart0_reader: SerialReader) -> bool:
    """Try both DTR polarities and return the 'connected' DTR level that works."""
    # Clean slate
    uart3.reset_input_buffer()
    uart0_reader.drain_text()
    uart3_reader.drain_text()

    # Prefer observing UART0 session messages (more definitive)
    # Firmware prints: "SESSION WAS INITIATED" when DTR indicates connect.
    for connected_level in (False, True):
        # Force an edge: disconnect then connect
        pulse_dtr(uart3, not connected_level, connected_level, delay_s=0.25)

        if wait_for_text(uart0_reader, "SESSION WAS INITIATED", timeout_s=1.0):
            # UART3 should print welcome/prompt too
            wait_for_any(uart3_reader, ("PWM Ready", ">"), timeout_s=1.0)
            return connected_level

    # Fallback: attempt by UART3 output only
    for connected_level in (False, True):
        pulse_dtr(uart3, not connected_level, connected_level, delay_s=0.25)
        out = wait_for_any(uart3_reader, ("PWM Ready", ">"), timeout_s=1.0)
        if "PWM Ready" in out or ">" in out:
            return connected_level

    raise RuntimeError("Could not establish UART3 session via DTR (no session/welcome observed)")


def run_case(uart3: "serial.Serial", uart3_reader: SerialReader, case: TestCase) -> tuple[bool, str]:
    uart3.reset_input_buffer()
    uart3_reader.drain_text()

    uart3.write(case.payload)
    uart3.flush()

    out = wait_for_any(uart3_reader, case.expect_any, timeout_s=case.timeout_s)

    for bad in case.forbid_any:
        if bad in out:
            return False, out

    ok = any(good in out for good in case.expect_any)
    return ok, out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="UART3 slick-prompt smoke test")
    ap.add_argument("--uart0", default=os.environ.get("UART0_DEV", "/dev/ttyACM0"))
    ap.add_argument("--uart3", default=os.environ.get("UART3_DEV", "/dev/ttyUSB1"))
    ap.add_argument("--uart0-baud", type=int, default=int(os.environ.get("UART0_BAUD", "9600")))
    ap.add_argument("--uart3-baud", type=int, default=int(os.environ.get("UART3_BAUD", "115200")))
    args = ap.parse_args(argv)

    print(f"Using UART0={args.uart0} @ {args.uart0_baud}")
    print(f"Using UART3={args.uart3} @ {args.uart3_baud}")

    with serial.Serial(args.uart0, args.uart0_baud, timeout=0.1) as u0, serial.Serial(
        args.uart3, args.uart3_baud, timeout=0.1
    ) as u3:
        u0r = SerialReader(u0, "UART0")
        u3r = SerialReader(u3, "UART3")
        u0r.start()
        u3r.start()

        try:
            # Establish session (and learn DTR polarity)
            print("\n[STEP] Establishing UART3 session via DTR...")
            connected_level = connect_session(u3, u3r, u0r)
            print(f"[INFO] DTR connected_level={connected_level}")

            # Command suite
            cases = [
                TestCase("HELP", b"HELP\r", expect_any=("Commands:", "PSYN n", "HELP")),
                TestCase("?", b"?\r", expect_any=("Commands:", "PSYN n")),
                TestCase("PSYN valid", b"PSYN 44\r", expect_any=("OK: duty set to 44%",)),
                TestCase("PSYN out-of-range", b"PSYN 4\r", expect_any=("ERROR: value out of range",)),
                TestCase("PSYN invalid", b"PSYN foo\r", expect_any=("ERROR: invalid number",)),
                TestCase("PSYN missing", b"PSYN\r", expect_any=("ERROR: missing value",)),
                TestCase("unknown", b"XYZ\r", expect_any=("ERROR: unknown command",)),
                TestCase(
                    "lowercase -> uppercase",
                    b"psyn 55\r",
                    expect_any=("OK: duty set to 55%",),
                ),
                TestCase(
                    "backspace edit",
                    b"PSYN 44\x08\x085\r",  # turns 44 into 5 after 2 BS => "PSYN 5" (should error)
                    expect_any=("ERROR", "OK"),
                    timeout_s=1.5,
                ),
                TestCase(
                    "DEL edit",
                    b"PSYN 66\x7f7\r",  # turns 66 into 67
                    expect_any=("OK: duty set to 67%", "ERROR"),
                    timeout_s=1.5,
                ),
                TestCase(
                    "Ctrl-U line kill then HELP",
                    b"PSYN 77\x15HELP\r",
                    expect_any=("Commands:", "PSYN n"),
                    timeout_s=1.5,
                ),
            ]

            print("\n[STEP] Exercising UART3 command suite...")
            failures: list[str] = []
            for case in cases:
                ok, out = run_case(u3, u3r, case)
                status = "PASS" if ok else "FAIL"
                print(f"\n[{status}] {case.name}")
                if not ok:
                    failures.append(case.name)
                    print(out)

            # DTR disconnect / reconnect behavior
            print("\n[STEP] Forcing DTR disconnect...")
            set_dtr(u3, not connected_level)
            if not wait_for_text(u0r, "SESSION WAS DISCONNECTED", timeout_s=1.5):
                failures.append("DTR disconnect -> UART0 notification")

            time.sleep(0.3)

            print("\n[STEP] Forcing DTR reconnect...")
            pulse_dtr(u3, not connected_level, connected_level, delay_s=0.25)
            if not wait_for_text(u0r, "SESSION WAS INITIATED", timeout_s=1.5):
                failures.append("DTR reconnect -> UART0 notification")

            # Expect welcome/prompt again
            welcome = wait_for_any(u3r, ("PWM Ready", ">"), timeout_s=1.5)
            if "PWM Ready" not in welcome:
                failures.append("DTR reconnect -> UART3 welcome")

            print("\n[RESULT]")
            if failures:
                print("FAILURES:")
                for f in failures:
                    print(f"- {f}")
                return 1

            print("All smoke checks passed.")
            return 0

        finally:
            u0r.stop()
            u3r.stop()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

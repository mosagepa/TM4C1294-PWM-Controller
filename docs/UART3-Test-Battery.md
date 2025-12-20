# UART3 Test Battery (Lab Smoke Tests)

This document is the “remembrance” for how we validate the UART3 slick prompt UX and DTR-based session behavior on the TM4C1294 controller.

## Canonical Port Mapping (keep for the record)

- UART0 (ICDI diagnostics): 9600 baud, TI ICDI serial, typically `/dev/ttyACM0` (or `/dev/serial/by-id/usb-Texas_Instruments_In-Circuit_Debug_Interface_*`).
- UART3 (USER console): 115200 baud, external USB-UART (FTDI), typically `/dev/ttyUSB1` (or `/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_*`).

The Makefile defaults prefer `/dev/serial/by-id/...` to avoid USB renumbering.

## Automated Smoke Test

Script: `tools/uart_smoketest.py`

### What it validates

- UART3 session establishment produces welcome + prompt
- UART3 command handling:
  - `HELP`, `?`
  - `PSYN n` valid
  - `PSYN` missing arg
  - `PSYN` invalid arg
  - `PSYN` out-of-range
  - unknown command
- Basic line-editing keystrokes (best-effort): Backspace, DEL, Ctrl-U
- DTR disconnect/reconnect behavior (best-effort):
  - UART0 prints `SESSION WAS DISCONNECTED` / `SESSION WAS INITIATED`
  - UART3 reprints welcome/prompt on reconnect

### Requirements / assumptions

- UART3 RX/TX must be correctly wired to TM4C UART3 pins (PJ0/PJ1).
- DTR-to-PQ1 wiring must exist *and* the USB-UART adapter/driver must support DTR toggling.
  - If DTR is not wired or not toggleable, the script can still validate command parsing but DTR tests may fail.

### Run

From the project root:

- Flash current firmware (sudo-less if udev is configured):
  - `make flash SUDO=`
- Run smoke test:
  - `python3 tools/uart_smoketest.py`

Optional overrides:

- `UART0_DEV`, `UART0_BAUD`, `UART3_DEV`, `UART3_BAUD` environment variables.

## Manual checks (when debugging lab setup)

- Verify UART3 echo: open miniterm and type; firmware should echo uppercase-as-you-type.
  - `python3 -m serial.tools.miniterm /dev/ttyUSB1 115200`
- Verify DTR effect: toggle DTR (miniterm menu) and watch UART0 for session messages.

## Updating this document

If `tools/uart_smoketest.py` changes (new commands, new expectations, different DTR semantics), update this file in the same commit so the “test battery” stays accurate.

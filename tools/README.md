# Tools

This folder contains small helper scripts to automate common workflows:

- Build + flash (via `make flash`)
- UART capture on UART0 (ICDI) and UART3 (USER)
- Optional command send to UART3 (e.g. `PSYN 44\r`)

## UART capture

Capture both UARTs for 10 seconds:

```bash
python3 tools/uart_session.py
```

Send a command to UART3 shortly after start:

```bash
python3 tools/uart_session.py --send-uart3 'PSYN 44\r' --duration 8
```

Override device nodes (or set env vars `UART0_DEV`, `UART3_DEV`):

```bash
python3 tools/uart_session.py --uart0 /dev/ttyACM0 --uart3 /dev/ttyUSB1
```

Logs are written to `./logs/`.

## Avoiding `/dev/ttyUSB0` collisions (Logic Analyzer vs UART3)

Linux assigns `/dev/ttyUSB*` numbers based on **enumeration order**. If you plug a Kingst logic analyzer (or any other USB-serial device) at the same time as the board’s UART3 adapter, the numbering can swap (UART3 might become `/dev/ttyUSB1` today, `/dev/ttyUSB0` tomorrow).

The fix is: **stop using `/dev/ttyUSB0` directly** and use a stable path.

### Recommended: use `/dev/serial/by-id/...`

List the stable names:

```bash
ls -l /dev/serial/by-id/
```

Then use the specific FTDI adapter for UART3, for example:

```bash
python3 tools/uart_session.py \
	--uart0 /dev/serial/by-id/usb-Texas_Instruments_In-Circuit_Debug_Interface_*-if00 \
	--uart3 /dev/serial/by-id/usb-FTDI_*_UART_*-port0
```

This works even if the logic analyzer steals `/dev/ttyUSB0`.

### Optional: udev symlinks (human-friendly names)

If you want fixed names like `/dev/uart3_ftdi` and `/dev/logic_analyzer`, create a udev rule on dijkstra.

1) Identify each device attributes (run for the *current* node):

```bash
udevadm info -a -n /dev/ttyUSB0 | sed -n '1,120p'
udevadm info -a -n /dev/ttyUSB1 | sed -n '1,120p'
```

2) Create a rule file (example template) at `/etc/udev/rules.d/99-serial-names.rules` matching each device’s `idVendor`, `idProduct`, and preferably serial number, then assign a symlink.

3) Reload rules:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

### If you see “device reports readiness to read but returned no data”

That pyserial/miniterm error usually means the OS believes the device is readable, but the USB ACM/USB-serial endpoint disappeared (disconnect/reset) or another process grabbed the port. Check:

```bash
dmesg | tail -n 200
lsof /dev/ttyACM0 /dev/ttyUSB0 /dev/ttyUSB1 2>/dev/null
```

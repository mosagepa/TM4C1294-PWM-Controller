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

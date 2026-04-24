# TACH Sensing Progress Report (2026-01-09)

## Scope

Goal: add an interrupt-driven TACH input (open-collector fan tach) to the TM4C1294XL firmware, with a command to print implied RPM periodically on UART0 (ICDI) while preserving the existing PWM behavior.

This report documents the state of implementation and the key lab caveats discovered so far.

## Summary of what’s implemented

### 1) GPIO TACH input + interrupt pulse counting

- TACH input is configured as a GPIO input with internal weak pull-up (`GPIO_PIN_TYPE_STD_WPU`, pulls to 3.3V).
- Falling-edge interrupts increment a pulse counter.
- Default pin chosen for convenience on the LaunchPad header: **PM3** (GPIOM3).

### 2) Periodic RPM reporting every 0.5s on UART0

- A `TACHIN` command on UART3 enables/disables periodic reporting.
- When enabled, firmware prints a line every 0.5s on UART0 (ICDI):

  `TACH pulses=<n> rejects=<n> rpm=<n>`

- RPM conversion used (per the project model):

  - `RPM = pulses_per_sec * 30`
  - Over a 0.5s window: `pulses_per_sec = 2 * pulses_in_window`
  - Therefore: `RPM = 60 * pulses_in_window`

### 3) Non-blocking timebase (SysTick)

- SysTick is enabled at 1ms tick to support clean 0.5s scheduling.
- A derived 32-bit cycle counter (based on SysTick) is provided for short delta measurements.

### 4) Glitch/noise mitigation added after abnormal lab readings

Observed in lab: extremely large implied RPM values (~1,000,000 RPM equivalent) that do not track PSYN intuitively.

Interpretation: tach input was likely seeing **PWM-coupled noise** or **fast glitches**, causing the edge counter to count events near tens of kHz rather than the true tach signal.

Mitigation implemented:

- ISR-level **minimum edge spacing** filter.
- Any edge arriving closer than `TACH_MIN_EDGE_US` (default 200µs) is rejected and counted in `rejects`.

This should suppress coupling from the ~24.9kHz PWM regime (period ~40µs).

## Files and integration points

- `tach.c/.h`
  - `tach_init()` configures PM3 + interrupts
  - `GPIOMIntHandler()` counts pulses (with glitch reject)
  - `tach_task()` prints to UART0 every 0.5s when enabled

- `timebase.c/.h`
  - SysTick ms counter + `timebase_cycles32()`

- `commands.c`
  - Adds `TACHIN [ON|OFF]` command

- `main.c`
  - Calls `timebase_init()` and `tach_init()` once
  - Calls `tach_task()` inside the active session loop

- `TM4C1294XL_startup.c`
  - Hooks vectors for SysTick and GPIO Port M

## Electrical and measurement caveats (must-read)

### 1) Voltage level / pull-up

- TM4C GPIO input is **3.3V tolerant** (do not apply 5V).
- If the fan tach is open-collector and was previously pulled up to **+5V** via 4k7, do **not** feed that directly into PM3.
- Preferred approach for bring-up:
  - Remove +5V pull-up
  - Use internal WPU (3.3V) or add external pull-up to **3.3V** (often stronger than internal)

Important measurement note (DMM averaging):

- A DMM reading near ~1.65V on a running tach line does **not** prove the pull-up rail is 3.3V.
- For a 0↔3.3V waveform at 50% duty, the average is ~1.65V, but other combinations can yield similar averages.
- To identify the pull-up rail, use a scope (DC-coupled) and read the actual **Vhigh** of the tach line.

### 1b) Safe interface to an unknown pull-up rail

If the PSU tach input might be pulled up to +5V (or uses an “active pull-up”), the safest interface is an external open-collector stage:

- 2N3904 (or similar NPN) as open-collector: emitter→GND, collector→PSU tach input, PSU pull-up left as-is.
- Base driven from the TM4C GPIO through a resistor (typ. 2.2k–4.7k), plus a weak base-emitter pulldown (47k–100k).

This keeps the TM4C pin isolated from the PSU pull-up voltage and still presents the PSU with a fan-like “sink-to-ground” tach signal.

### 2) EMI coupling from PWM

- PWM at ~21.5kHz can capacitively/inductively couple into a high-impedance open-collector tach line.
- Long wires and breadboard jumpers can worsen this.
- Cheap scopes can show “aliasing/mangling” that makes tach edges appear distorted.

### 3) What the diagnostics mean

- `pulses` is the number of accepted falling edges in the last 0.5s.
- `rejects` is the number of edges rejected for being too fast (likely noise).

Expected qualitative behaviors:

- If `rejects` is very large: noise dominates → consider stronger 3.3V pull-up and/or RC filtering.
- If `pulses` stays near 0: no edges → check wiring, common ground, pin selection, or fan tach availability.

## Next lab checklist (for 2026-01-10)

1) Verify PM3 wiring, and ensure **no +5V pull-up** is present.
2) Confirm fan ground and LaunchPad ground are common.
3) Run `TACHIN ON`, then step PSYN through a few values.
4) Record a few lines of UART0 output including both `pulses` and `rejects`.
5) If needed, tune the glitch filter (e.g., try 100–1000µs) by changing `TACH_MIN_EDGE_US`.

## Additional Notes

New lab notes (2026-01-10) indicate the physical IBM PS tach line is *bursty* (bursts of ~21.5kHz pulses followed by a low tail), which can alias badly with naive fixed-window counting.
See: [LEEME_MOSA_TACH_ANALYSIS.TXT](../LEEME_MOSA_TACH_ANALYSIS.TXT)

### IBM boot-time tach expectation (observed)

Later lab work (2026-01-11) observed an additional “expected tach behavior” sequence during cold AC-power boot, likely used by the server/PSU to validate fan presence.

All values below are approximate and should be treated as a working hypothesis until re-captured and confirmed:

- ~0 to 4.5s: tach frequency transitions from ~60Hz down to ~50Hz (50% duty)
- ~4.5 to 7s: tach holds near ~50Hz (50% duty)
- ~17 to 28s: tach ramps from ~50Hz up to ~168Hz (50% duty)

This sequence is distinct from the later steady-state “phase 1” (~168Hz) and “phase 2” (~235–236Hz) regimes.

### Plan (based on lab notes)

Add a `TSYN ON|OFF` mode that drives **PM3** with a clean, synthesized “tach-like” burst waveform whose parameters are interpolated from the lab table vs the currently requested `PSYN n`.
Implement the 21.5kHz carrier using a hardware timer output, and only use interrupts at burst boundaries (low interrupt rate).
While TSYN is ON, disable tach capture interrupts on PM3 to avoid self-triggering.
---

Generated: 2026-01-09

---

# Update Appendix (2026-04-24)

This appendix captures changes and new mechanics added after the original 2026-01-09 report. The original content above is preserved verbatim for historical continuity.

## 1) Actual current pinning / ISR (integr_v03)

Since the 2026-01-09 snapshot, the integrated firmware moved tach capture off Port M and onto **GPIO Port F**:

- Default tach input pin: **PF1** (`TACH_GPIO_BASE=GPIO_PORTF_BASE`, `TACH_GPIO_PIN=GPIO_PIN_1`)
- ISR vector: **`GPIOFIntHandler()`** in `tach.c` (not `GPIOMIntHandler()`)

A key integration detail: Port F ISR also conditionally services **PWMIN fallback capture on PF3** by calling `pwmin_gpiof_isr()` when PF3 interrupts are enabled.

Implication: PF1 (TACH) and PF3 (PWMIN fallback) share the same GPIO port ISR and must stay lightweight.

## 2) TACH reporting format refinements

The periodic 0.5 s `TACHIN` prints were extended beyond the original minimal format.

Current behavior when `TACHIN ON` is enabled:

- A one-time header is printed on UART0 describing pin + edge configuration.
- Each 0.5 s report includes:
  - `pulses=<n>` / `rejects=<n>`
  - `f=<Hz.t>Hz` (0.1 Hz resolution, window-normalized)
  - optional loopback OK/FAIL hints when the self-test expected frequency is configured
  - `rpm=<n>` (integer)

## 3) PWMINDBG timestamps (HH:MM:SS) on UART0

`PWMINDBG ON` now causes the per-second verbose line to be **prefixed** with an uptime timestamp:

- `<HH:MM:SS> PWMIN DBG: f=<Hz.t>Hz duty=<pct.t>% period_cycles=<n> high_cycles=<n> edges=<n>`

The timestamp is uptime-based (derived from `timebase_millis()`), intended for correlating bench logs.

## 4) Regime-change warnings (strictly gated by TACHIN)

When **both** of these are true:

- `PWMINDBG ON` (verbose PWMIN diagnostics enabled), and
- `TACHIN ON` (tach reporting active)

…the firmware monitors for **relative changes > 15%** in either:

- PWM duty (integer %), and/or
- Tach RPM (integer)

If such a change is detected, then on the **next** 1 Hz verbose tick a warning block is emitted (on UART0) above the usual `PWMIN DBG:` line:

- `<HH:MM:SS> *** WARNING - REGIME CHANGE ****`
- `PREV. REGIME <HH:MM:SS> --> NEW REGIME: <HH:MM:SS>`
- `PWM DUTY ---> XX %TO YY % ;  TACH RPM ---> XXXXX to YYYYY`

Operational notes:

- If `TACHIN` is NOT active, **no regime-change warnings are printed**, even if duty changes.
- The regime baseline timestamp is seeded at the moment `PWMINDBG ON` is issued.
- When `TACHIN` becomes active, the first valid RPM sample seeds the RPM baseline without triggering a warning.

## 5) Recommended operator sequence for "timestamped regime" monitoring

To use the new regime-warning mechanics during bench logging:

1) `PWMINDBG ON`
2) `TACHIN ON`
3) Apply your stimulus (PSU demand changes, load changes, etc.)

This yields timestamped `PWMIN DBG` lines and, only when TACHIN is active, explicit regime change markers.

---

Appended: 2026-04-24

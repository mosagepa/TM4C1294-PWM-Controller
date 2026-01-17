# TM4C1294-PWM-Controller

A comprehensive PWM control system for the Texas Instruments TM4C1294XL Tiva C ARM Cortex-M4 microcontroller, featuring custom memory management and UART-based command interface.

> 📚 **Complete Technical Documentation**: For detailed development insights, comprehensive hardware specifications, platform details, implementation guides, and the full project evolution (ATTiny45 → ESP32-S2 → TM4C1294XL), please visit our **[Technical Documentation Center](./docs/README.md)**.

> 🤖 **AI Collaboration Insights**: See [GHCP_COMMENTS.md](./GHCP_COMMENTS.md) for detailed AI-assisted development experiences, technical insights, and problem-solving approaches used in this project. This resource documents the embedded systems development journey and serves as a learning guide for similar projects.

## 🚀 Features

- **PWM Control**: PF2 PWM at ~24.9kHz with configurable duty cycle (5-96%)
- **Tach I/O Harness**:
  - TACH OUT on PM3 (continuous square-wave generation, boot-profile simulation, or copy/mirror)
  - TACH IN on PF1 (edge-counting RPM diagnostics, optional loopback/self-test)
- **PWM Input Measurement**: PF3 capture with periodic UART0 reporting (`PWMIN ON/OFF`)
- **Dual UART Interface**: 
  - UART0 (ICDI): 9600 baud diagnostic output
  - UART3 (USER): 115200 baud command input
- **Custom Memory Management**: Heap-based allocation with `malloc_simple.c`
- **Diagnostic System**: Comprehensive memory and variable inspection via `diag_uart.c`
- **Session Detection**: DTR-based session management on PQ1
- **Command Processing**: Real-time command parsing and execution

## 🔧 Hardware Requirements

- **Microcontroller**: TM4C1294XL Tiva C Series Launchpad
- **PWM Output**: PF2 (~24.9kHz)
- **PWM Input (optional)**: PF3
- **Tach Output**: PM3
- **Tach Input**: PF1
- **Debug Interface**: ICDI (Integrated Circuit Debug Interface)
- **External UART**: UART3 on dedicated pins for command input

## 📦 Project Structure

```
├── main.c                    # Main application with PWM control and testing
├── diag_uart.h/c            # Custom diagnostic and sprintf replacement functions
├── cmdline.h/c              # UART command line interface
├── syscalls.c               # System call implementations
├── malloc_simple.c          # Custom heap memory allocator
├── TM4C1294XL_startup.c     # Hardware initialization and startup code
├── TM4C1294XL.ld           # Linker script with memory layout
├── drivers/                 # Hardware abstraction layer
│   ├── pinout.h/c          # Pin configuration and mapping
│   └── [other drivers]     # Additional peripheral drivers
└── Makefile                # Build system configuration
```

## 🛠️ Build System

**Requirements:**
- ARM GCC Toolchain (`arm-none-eabi-gcc`)
- TivaWare DriverLib
- OpenOCD (for flashing)

**Build Commands:**
```bash
make clean          # Clean build artifacts
make                # Compile project
make flash          # Flash firmware to target
make reset          # Reset target microcontroller
```

**Memory Usage:**
- Flash: ~25KB (linker-reported `.text` size varies with debug flags)
- SRAM: ~10KB used out of a configured 32KB window

### SRAM Usage Note (Heap Reservation)

The linker script reserves a `.heap` region in SRAM (NOLOAD). This affects the linker's
`--print-memory-usage` report:

- **Before (Jan 2026)**: `.heap` was reserved as ~22KB, so the linker reported ~75% SRAM usage.
- **Now**: `.heap` is reserved as **8KB**, and the linker reports ~31% SRAM usage.

**Impact**:
- `malloc()` (via `_sbrk`) can grow only up to the reserved heap size; if you exceed it,
  allocations fail with `ENOMEM`.
- The remaining unreserved SRAM becomes a **guard band** between heap and the stack-reserved
  top-of-SRAM region, reducing accidental heap/stack collision risk.

**How to tune**: edit `_heap_size` in `TM4C1294XL.ld` and rebuild.

## 🎮 Usage

### Command Interface (UART3 @ 115200)

Core PWM:

- `PSYN n` (n=5..96)
- `PSYN ON` / `PSYN OFF`

IBM PSU mimic presets (PF2 PWM + PM3 TACHSYN):

- `PHASE1` / `PHASE 1`   (46% + 168Hz)
- `PHASE2` / `PHASE 2`   (54% + 235Hz)
- `PHASE1L` / `PHASE 1L` (15% + 168Hz)
- `PHASE2L` / `PHASE 2L` (21% + 235Hz)

Boot-profile tach simulation (PM3):

- `TSYN BOOT BEGIN` / `TSYN BOOT END`

Tach copy/mirror mode (PF1 → PM3):

- `TSYN COPY BEGIN` / `TSYN COPY END`

Persistent defaults (EEPROM):

- `TACH DEFAULT <1|2|1L|2L|BOOT|COPY>`
- `TACH DEFAULT CURRENT`

Diagnostics (UART0 @ 9600):

- `TACHIN ON/OFF` (prints RPM/pulses/rejects)
- `PWMIN ON/OFF` (prints PWM-in freq/duty)
- `TACH LOOPBACK BEGIN/END` (jumper PM3 → PF1)

Notes:

- On boot, the firmware loads/apply `TACH DEFAULT` from EEPROM (fallback: `PHASE1L`).
- `TSYN COPY END` and `TACH LOOPBACK END` automatically restore the persisted `TACH DEFAULT`.

### Monitoring

Connect to ICDI UART at 9600 baud to monitor:
- System initialization messages
- Memory allocation diagnostics
- Variable state information
- Error conditions and debugging output

## 🔍 Key Features Deep Dive

### Custom Memory Management
- **Heap Implementation**: `malloc_simple.c` provides custom heap allocation
- **Memory Diagnostics**: Real-time heap usage monitoring
- **Stack Protection**: Proper stack/heap boundary management

### Diagnostic System
- **Variable Inspection**: `diag_print_variable()` for runtime state analysis
- **Memory Visualization**: Hex dump capabilities for debugging
- **Printf Replacement**: Custom `sprintf()` family avoiding standard library issues

### PWM Generation
- **Hardware Timer**: Utilizes TM4C1294 timer peripherals
- **Frequency Control**: Configurable base frequency (default 21.5kHz)
- **Duty Cycle Range**: 5-96% with 1% resolution
- **Real-time Updates**: Immediate response to command changes

## ⚡ Development Status

### Current Implementation
- ✅ Hardware initialization and PWM generation
- ✅ UART command interface with error handling
- ✅ Custom memory management system
- ✅ Comprehensive diagnostic framework
- ✅ Build system and flashing tools

### Known Issues
- 🔧 **sprintf Replacement**: Current implementation causes runtime stalls
- 🔧 **Standard Library**: `snprintf()` conflicts with custom memory management

### Roadmap
- [ ] Resolve sprintf/snprintf runtime stall issue
- [ ] Implement simpler string formatting without heap allocation
- [ ] Add more PWM control commands (frequency adjustment)
- [ ] Extend diagnostic capabilities
- [ ] Add configuration persistence

## 🧪 Testing

### Hardware Testing Flow
1. Flash firmware to TM4C1294XL
2. Monitor ICDI UART for boot diagnostics
3. Send commands via UART3 interface
4. Verify PWM output on oscilloscope

### Test Commands
```bash
# Monitor ICDI output (9600 baud)
python3 -m serial.tools.miniterm /dev/ttyACM0 9600 --raw

# Send commands (115200 baud)
python3 -m serial.tools.miniterm /dev/ttyUSB1 115200 --raw
```

## 📚 Technical Details

### Memory Map
- **Flash**: 0x00000000 - 0x00100000 (1MB)
- **SRAM (hardware)**: 0x20000000 - 0x2003FFFF (256KB)
- **SRAM (this firmware build)**: currently links into a 32KB SRAM window (see `TM4C1294XL.ld`)
- **Heap**: bounded by the reserved `.heap` region in the linker script
- **Stack**: top-of-window stack region reserved by linker symbols (grows downward at runtime)

### UART Configuration
- **UART0 (ICDI)**: 9600-8-N-1, diagnostic output only
- **UART3 (USER)**: 115200-8-N-1, bidirectional commands

### Pin Assignments
- **PF2**: PWM output
- **PQ1**: DTR session detection
- **UART3**: External command interface

## 🤝 Contributing

This project is part of an embedded systems development effort. Contributions welcome for:
- String formatting improvements
- Additional PWM features
- Enhanced diagnostics
- Code optimization

## 📄 License

This project contains code for embedded systems development and educational purposes.

## 🔗 Related Documentation

- [TM4C1294XL Datasheet](https://www.ti.com/product/TM4C1294NCPDT)
- [TivaWare Peripheral Driver Library](https://www.ti.com/tool/SW-TM4C)
- [ARM Cortex-M4 Documentation](https://developer.arm.com/Processors/Cortex-M4)

Command and firmware behavior details:

- [docs/Firmware-Function-Reference.md](docs/Firmware-Function-Reference.md)

---

**Project Status**: Active Development | **Target**: TM4C1294XL | **Language**: C99
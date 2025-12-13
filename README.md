# TM4C1294-PWM-Controller

A comprehensive PWM control system for the Texas Instruments TM4C1294XL Tiva C ARM Cortex-M4 microcontroller, featuring custom memory management and UART-based command interface.

> 🤖 **AI Collaboration Insights**: See [GHCP_COMMENTS.md](./GHCP_COMMENTS.md) for detailed AI-assisted development experiences, technical insights, and problem-solving approaches used in this project. This resource documents the embedded systems development journey and serves as a learning guide for similar projects.

## 🚀 Features

- **PWM Control**: Precise PWM generation with configurable duty cycle (5-96%)
- **Dual UART Interface**: 
  - UART0 (ICDI): 9600 baud diagnostic output
  - UART3 (USER): 115200 baud command input
- **Custom Memory Management**: Heap-based allocation with `malloc_simple.c`
- **Diagnostic System**: Comprehensive memory and variable inspection via `diag_uart.c`
- **Session Detection**: DTR-based session management on PQ1
- **Command Processing**: Real-time command parsing and execution

## 🔧 Hardware Requirements

- **Microcontroller**: TM4C1294XL Tiva C Series Launchpad
- **PWM Output**: PF2 (21.5kHz target frequency)
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
- Flash: ~41KB
- SRAM: ~24KB

## 🎮 Usage

### Command Interface

Connect to UART3 at 115200 baud and use the following commands:

```
PSYN n    # Set PWM duty cycle (n = 5 to 96)
```

**Example:**
```
PSYN 50   # Set 50% duty cycle
PSYN 25   # Set 25% duty cycle
PSYN 80   # Set 80% duty cycle
```

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
- **SRAM**: 0x20000000 - 0x20040000 (256KB)
- **Heap**: Dynamically managed via custom allocator
- **Stack**: High SRAM addresses, grows downward

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

---

**Project Status**: Active Development | **Target**: TM4C1294XL | **Language**: C99
# 🔧 Hardware Requirements - Complete Technical Specification

This document provides comprehensive hardware specifications, platform details, and technical references for the TM4C1294-PWM-Controller system.

## Table of Contents
- [TM4C1294XL Platform Overview](#tm4c1294xl-platform-overview)
- [Detailed Hardware Specifications](#detailed-hardware-specifications)
- [Pin Assignments and Connections](#pin-assignments-and-connections)
- [Peripheral Utilization](#peripheral-utilization)
- [External Hardware Requirements](#external-hardware-requirements)
- [Development Tools](#development-tools)

---

## TM4C1294XL Platform Overview

### EK-TM4C1294XL Connected LaunchPad

The Texas Instruments EK-TM4C1294XL is a comprehensive development platform featuring the powerful TM4C1294NCPDT microcontroller. This platform provides the ideal foundation for high-performance embedded applications requiring precise timing and comprehensive connectivity.

#### Official Product Information
- **TI Product Page**: [EK-TM4C1294XL LaunchPad](https://www.ti.com/tool/EK-TM4C1294XL)
- **User Guide**: [SPMU373C - EK-TM4C1294XL User's Guide](https://www.ti.com/lit/pdf/spmu373)
- **Schematic**: [EK-TM4C1294XL Design Files (Rev D)](https://www.ti.com/lit/zip/SPMR241)

### Core Microcontroller: TM4C1294NCPDT

#### Processor Specifications
- **CPU Core**: ARM Cortex-M4F with floating-point unit
- **Operating Frequency**: 120 MHz maximum
- **Architecture**: 32-bit RISC with Thumb-2 instruction set
- **Performance**: 150 DMIPS at 120 MHz
- **Floating Point**: IEEE 754 single-precision FPU

#### Memory Configuration
- **Flash Memory**: 1024 KB (1 MB) program memory
- **SRAM**: 256 KB total system RAM
- **EEPROM**: 6 KB non-volatile data storage
- **ROM**: 128 KB with TivaWare DriverLib and boot loader

#### Memory Map Detail
```
Flash Memory:    0x00000000 - 0x000FFFFF (1024 KB)
SRAM:           0x20000000 - 0x2003FFFF (256 KB)
Bit-band SRAM:  0x22000000 - 0x23FFFFFF (32 MB alias)
Peripherals:    0x40000000 - 0x5FFFFFFF
Private Periph: 0xE0000000 - 0xE00FFFFF
EEPROM:         0x400AF000 - 0x400AFFFF (6 KB)
```

### Official TI Documentation References

#### Primary Datasheets
- **[TM4C1294NCPDT Datasheet](https://www.ti.com/lit/ds/symlink/tm4c1294ncpdt.pdf)**: Complete microcontroller specifications
- **[TivaWare Peripheral Driver Library User's Guide](https://www.ti.com/lit/ug/spmu298e/spmu298e.pdf)**: Software library documentation
- **[ARM Cortex-M4 Technical Reference Manual](https://developer.arm.com/documentation/100166/0001/)**: Core processor details

#### Application Notes
- **[System Design Guidelines for TM4C129x Family](https://www.ti.com/lit/pdf/spma097)**: Hardware design best practices
- **[TM4C129x Ethernet Applications for lwIP](https://www.ti.com/lit/pdf/spma070)**: Network connectivity implementation
- **[TM4C129x Ethernet Applications for TI-RTOS NDK](https://www.ti.com/lit/pdf/spma069)**: Real-time network applications

---

## Detailed Hardware Specifications

### Power Requirements

#### Supply Specifications
- **Operating Voltage**: 3.3V nominal (3.0V to 3.6V range)
- **I/O Voltage**: 3.3V (some devices/pins may be 5V tolerant on *inputs*; treat pins as 3.3V unless verified)
- **Power Consumption**: 
  - Active mode (120 MHz): ~140 mA typical
  - Sleep mode: < 1 mA
  - Deep sleep: < 500 μA

#### LaunchPad Power Options
1. **USB Power**: Via micro-USB connector (5V → 3.3V regulation)
2. **External Power**: 7-15V DC via barrel jack (regulated to 3.3V)
3. **BoosterPack Power**: 3.3V directly to BoosterPack connector

### Clock System

#### Primary Clock Sources
- **Main Oscillator**: 25 MHz crystal (Y1 on LaunchPad)
- **Internal Oscillator**: 16 MHz ±1% precision
- **RTC Oscillator**: 32.768 kHz for real-time clock (optional)

#### PLL Configuration
- **Input Range**: 5-25 MHz
- **VCO Range**: 320-480 MHz
- **Output**: Divided to achieve 120 MHz system clock
- **Jitter**: < 150 ps RMS

```c
// Clock configuration for 120 MHz operation
SysCtlClockFreqSet((SYSCTL_XTAL_25MHZ | SYSCTL_OSC_MAIN |
                    SYSCTL_USE_PLL | SYSCTL_CFG_VCO_480), 120000000);
```

### GPIO Capabilities

#### Port Configuration
- **Total GPIO Pins**: 90 pins across Ports A-Q
- **Drive Strength**: 2mA, 4mA, 8mA options
- **Special Features**: 
  - 5V tolerant inputs (selected pins; verify per datasheet)
  - Open-drain outputs
  - Internal pull-up/pull-down resistors
  - Interrupt capability on all pins

#### Port Distribution on LaunchPad
- **Port F**: User LEDs (PF0-PF4) and switches (PF0, PF4)
- **Port N**: RGB LED (PN0-PN1)
- **Port J**: User switches (PJ0-PJ1)
- **Port Q**: Additional I/O and special functions

---

## Pin Assignments and Connections

### Project pinning (what this firmware actually uses)

This section documents the *actual* signal wiring used by the current firmware.

| Signal | MCU pin | Peripheral / function | Notes |
|---|---|---|---|
| PWM output (to PSU) | PF2 | `M0PWM2` (PWM0 module, output 2) | Target is ~24.9kHz (`TARGET_PWM_FREQ_HZ 24900U`), duty via `PSYN` / `PHASE*`. 3.3V logic. |
| PWM input sense (from PSU) | PF3 | Intended `T1CCP1` (Timer1B capture), fallback GPIOF both-edge ISR | `PWMIN` reports `f` + `duty` at 1Hz on UART0. PF3 is **not** a user LED pin on this board (LEDs are PF0/PF4, PN0/PN1). |
| TACH input (fan tach sense) | PF1 | GPIO interrupt (falling edge + reject filter) | Uses internal weak pull-up; treat as **3.3V only** unless conditioned. |
| TACH output (fake tach to PSU) | PM3 | Timer-based tach synthesizer | Driven by `PHASE*`, `TSYN BOOT`, `TSYN COPY`. |
| UART0 (ICDI debug/log) | PA0/PA1 | `U0RX/U0TX` | Shows as `/dev/ttyACM*` on Linux. |
| UART3 (user command port) | PA4/PA5 | `U3TX/U3RX` | Typically via external USB-serial adapter (`/dev/ttyUSB*`), 3.3V logic. |
| Session detect (DTR) | PQ1 | GPIO input | Used to detect terminal connect/disconnect boundary. |

### UART Interface Pins

#### UART0 (ICDI Debug Interface)
- **TX**: PA1 (connected to ICDI via U5 level shifter)
- **RX**: PA0 (connected to ICDI via U5 level shifter)
- **Connector**: USB micro-B (virtual COM port)
- **Isolation**: Galvanically isolated via ICDI circuitry

#### UART3 (External Command Interface)
- **TX**: PA4 (BoosterPack pin 12 - J2)
- **RX**: PA5 (BoosterPack pin 11 - J2)  
- **Voltage**: 3.3V CMOS logic levels
- **Protection**: Series resistors R9/R10 (33Ω each)

```c
// UART3 pin configuration
GPIOPinConfigure(GPIO_PA4_U3TX);
GPIOPinConfigure(GPIO_PA5_U3RX);
GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_4 | GPIO_PIN_5);
```

#### Host-side device nodes: `/dev/ttyUSB0` vs `/dev/ttyUSB1`

Linux assigns `/dev/ttyUSB*` numbers by enumeration order, so the UART3 adapter can appear as `/dev/ttyUSB0` on one host and `/dev/ttyUSB1` on another.

Recommended solutions:

1) Use stable names under `/dev/serial/by-id/` (best).

- `ls -l /dev/serial/by-id/`
- Then pass that path into the tooling (examples):
  - `make capture UART3_DEV=/dev/serial/by-id/<your-uart3-adapter>`
  - `python3 tools/uart_session.py --uart3 /dev/serial/by-id/<your-uart3-adapter>`

2) Override Makefile variables per invocation (simple):

- `make capture UART3_DEV=/dev/ttyUSB0`
- `make auto UART3_DEV=/dev/ttyUSB1`

This avoids editing the Makefile per host.

### Session Detection Pin

#### DTR Detection: PQ1
- **Function**: Data Terminal Ready input
- **Location**: BoosterPack expansion area
- **Configuration**: Input with internal pull-up
- **Logic**: Active-low DTR signal detection

### Debug and Programming Interface

#### JTAG/SWD Connections (via ICDI)
- **SWDIO**: PC1 (Serial Wire Debug I/O)
- **SWCLK**: PC0 (Serial Wire Debug Clock)
- **TDO**: PC2 (Test Data Out)
- **TDI**: PC3 (Test Data In)
- **nTRST**: PC7 (Test Reset)

---

## Peripheral Utilization

### Timer Modules

#### PWM0 module - PWM generation (PF2)

In the current firmware, PWM output is produced by the **PWM peripheral** (not a TimerA PWM mode).

- **Pin**: PF2 (`M0PWM2`)
- **Module / generator**: `PWM0`, `PWM_GEN_1`, output `PWM_OUT_2`
- **Base frequency target**: **24.9 kHz** (`TARGET_PWM_FREQ_HZ 24900U`)
- **Duty cycle range**: 5–96% (enforced by command layer)
- **Clocking**: `PWM_SYSCLK_DIV_1` (uses system clock)

Representative configuration (from `main.c`):

```c
GPIOPinConfigure(GPIO_PF2_M0PWM2);
GPIOPinTypePWM(GPIO_PORTF_BASE, GPIO_PIN_2);

PWMClockSet(PWM0_BASE, PWM_SYSCLK_DIV_1);

PWMGenConfigure(PWM0_BASE, PWM_GEN_1, PWM_GEN_MODE_DOWN | PWM_GEN_MODE_NO_SYNC);
PWMGenPeriodSet(PWM0_BASE, PWM_GEN_1, period);
PWMPulseWidthSet(PWM0_BASE, PWM_OUT_2, pulse);
PWMOutputState(PWM0_BASE, PWM_OUT_2_BIT, true);
PWMGenEnable(PWM0_BASE, PWM_GEN_1);
```

Note: with a 120 MHz system clock, the period is quantized to an integer number of clock ticks. The firmware targets 24.9 kHz closely by computing `period = round(sysclk / 24900)`.

#### Available Additional Timers
- **Timer1-7**: 32-bit general purpose timers
- **Wide Timer0-5**: 64-bit timers for extended timing
- **Watchdog Timer**: System reliability monitoring

### UART Modules

#### UART0 - Debug Output
- **Base Address**: 0x4000C000
- **Interrupt**: UART0_IRQn (21)
- **DMA**: DMA channel assignment available
- **FIFO**: 16-byte transmit/receive FIFOs

#### UART3 - Command Interface  
- **Base Address**: 0x4000F000
- **Interrupt**: UART3_IRQn (24)
- **Features**: Full modem control signals available
- **Baud Rate**: Up to 1.8 Mbps theoretical maximum

### ADC Capabilities (Available for Future Enhancement)

#### ADC0 Module
- **Resolution**: 12-bit (4096 steps)
- **Sampling Rate**: Up to 2 MSPS
- **Input Channels**: 20 external + 4 internal
- **Reference**: Internal 3.0V or external VREFA

#### ADC1 Module
- **Identical Specifications**: Dual ADC for simultaneous sampling
- **Digital Comparators**: 8 programmable comparators
- **Temperature Sensor**: Internal temperature monitoring

---

## External Hardware Requirements

### Power Supply Considerations

#### Recommended External Power
- **Voltage**: 9-12V DC for optimal regulation efficiency
- **Current**: Minimum 500mA for full-speed operation
- **Connector**: 2.1mm center-positive barrel jack
- **Regulation**: On-board LM3671 (3.3V/600mA switching regulator)

#### USB Power Limitations
- **Current**: Limited to 500mA USB specification
- **Performance**: May affect high-speed operation under heavy load
- **Recommendation**: External power for production applications

### External UART Interface

#### Level Shifting Requirements
For interfacing with RS-232 devices:
- **IC Recommendation**: MAX3232 or equivalent
- **Supply**: 3.3V operation
- **Capacitors**: 4x 100nF ceramic for charge pump

#### USB-to-Serial Converter
For development convenience:
- **Recommended**: FTDI FT232R or CP2102-based modules
- **Voltage**: 3.3V logic level compatibility
- **Connections**: 
  - TXD → PA5 (UART3 RX)
  - RXD → PA4 (UART3 TX)
  - GND → GND

### PWM Output Interface

#### Direct 3.3V Logic Interface
- **Drive Current**: 8mA maximum per pin
- **Logic Levels**: 
  - VOH: 2.4V minimum @ IOH = -2mA
  - VOL: 0.4V maximum @ IOL = 2mA

#### Power Driver Interface
For high-current loads:
- **Buffer IC**: 74HC244 or similar CMOS buffer
- **MOSFET Driver**: For switching applications
- **Isolation**: Optocouplers for electrical isolation

---

## Development Tools

### Programming and Debug Tools

#### Integrated Circuit Debug Interface (ICDI)
- **Functionality**: On-board JTAG/SWD debugger
- **Processor**: TM4C123GH6PM dedicated debug MCU
- **Interface**: USB 2.0 Full Speed
- **Compatibility**: OpenOCD, Code Composer Studio, Keil μVision

#### External Debug Probes (Optional)
- **SEGGER J-Link**: Professional debugging solution
- **TI XDS110**: High-performance TI-specific probe
- **OpenOCD Compatible**: Various ARM-based debug probes

### Software Development Environment

#### Recommended Toolchains
1. **ARM GCC Toolchain**
   - Version: arm-none-eabi-gcc 10.3.1 or newer
   - Optimization: -Os for size, -O2 for performance
   - Debugging: -ggdb for GDB compatibility

2. **Code Composer Studio (CCS)**
   - TI's official IDE
   - Integrated debugging and profiling
   - Free for evaluation and educational use

3. **Keil μVision MDK-ARM**
   - Professional ARM development environment
   - Advanced debugging and trace capabilities
   - Commercial licensing required

#### Build System
- **Make**: Standard GNU Make with custom Makefile
- **CMake**: Modern build system (future consideration)
- **TivaWare**: Required peripheral driver library

---

## Performance and Timing Specifications

### Real-time Performance
- **Interrupt Latency**: < 16 clock cycles (133ns @ 120MHz)
- **Context Switch**: < 100 clock cycles typical
- **PWM Update**: < 1ms from command to output change

### Environmental Specifications
- **Operating Temperature**: -40°C to +85°C (industrial grade)
- **Storage Temperature**: -55°C to +150°C
- **Humidity**: 5% to 95% non-condensing
- **Vibration**: Per MIL-STD-883 (LaunchPad level)

### Electrical Characteristics
- **ESD Protection**: ±2kV HBM, ±200V MM
- **Latch-up Immunity**: ±200mA
- **Input Leakage**: ±1μA maximum @ 3.6V

This comprehensive hardware specification provides the complete technical foundation for understanding and extending the TM4C1294-PWM-Controller system. The EK-TM4C1294XL LaunchPad serves as an excellent development platform with extensive peripheral capabilities and professional development tool support.
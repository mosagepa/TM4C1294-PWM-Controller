# 📚 TM4C1294-PWM-Controller Documentation Index

This documentation provides comprehensive technical details about the TM4C1294-PWM-Controller system, expanding significantly on the information in the main README file.

## 📖 Documentation Structure

### Core Documentation
- **[README.md](../README.md)**: Project overview, quick start, and basic usage
- **[GHCP_COMMENTS.md](../GHCP_COMMENTS.md)**: Technical blog with AI collaboration insights

### Command & Behavior Reference

- **[Firmware Function Reference](./Firmware-Function-Reference.md)**: UART3 commands (PHASE/TSYN/TACH/PWMIN), tach modes (BOOT/COPY/LOOPBACK), and EEPROM-backed `TACH DEFAULT` behavior.

### Detailed Technical Documentation

#### 🧭 [Project Evolution - From ATTiny45 → ESP32-S2 → TM4C1294XL](./Project-Evolution.md)
End-to-end project history and migration rationale derived from the original development artifacts.

#### 🚀 [Features - Comprehensive Technical Overview](./Features.md)
Extensive analysis of all system features with implementation details:
- **PWM Control System**: Hardware timer configuration, mathematical foundation, performance characteristics
- **Dual UART Interface**: Detailed specifications, protocol analysis, usage contexts
- **Custom Memory Management**: malloc_simple.c implementation, heap structure, performance analysis
- **Diagnostic System**: diag_uart.c functions, custom sprintf implementation, debugging capabilities
- **Session Detection**: DTR-based session management, hardware implementation
- **Command Processing**: Real-time command engine, parsing state machine, performance metrics

#### 🔧 [Hardware Requirements - Complete Technical Specification](./Hardware-Requirements.md)
Comprehensive hardware specifications and platform details:
- **TM4C1294XL Platform Overview**: LaunchPad specifications, official references
- **Detailed Hardware Specifications**: Power, clock, GPIO capabilities
- **Pin Assignments**: PWM output, UART interfaces, session detection
- **Peripheral Utilization**: Timer modules, UART controllers, ADC capabilities
- **External Hardware**: Power supply, level shifting, PWM output interfacing
- **Development Tools**: Programming interfaces, software environments

#### 🔬 [TM4C1294XL Platform - Complete Technical Reference](./TM4C1294XL-Platform.md)
Exhaustive platform documentation with official TI references:
- **Platform Overview**: EK-TM4C1294XL Connected LaunchPad details
- **TM4C1294NCPDT Deep Dive**: ARM Cortex-M4F architecture, memory configuration
- **LaunchPad Board Architecture**: Power management, reset/clock generation, ICDI interface
- **Peripheral Analysis**: UART controllers, timer modules, PWM generators, ADC specifications
- **Pin Multiplexing**: Complete GPIO port distribution, alternate function mapping
- **Power Management**: Advanced power modes, consumption analysis
- **Clock Architecture**: Master clock system, PLL configuration, distribution
- **Connectivity**: BoosterPack ecosystem, Ethernet, USB, CAN interfaces

## 🎯 Documentation Purpose

### For Developers
- **Implementation Guidance**: Code references and technical rationale for design decisions
- **Optimization Insights**: Performance characteristics and timing specifications
- **Debugging Support**: Comprehensive diagnostic capabilities and troubleshooting approaches
- **Extension Framework**: Platform capabilities for adding new features

### For System Integrators  
- **Hardware Requirements**: Complete specifications for production deployment
- **Interface Details**: Pin assignments, electrical characteristics, connector information
- **Performance Metrics**: Real-time performance, power consumption, environmental specifications
- **Compatibility**: BoosterPack ecosystem and expansion options

### For Educational Use
- **Learning Resource**: Embedded systems design principles and ARM Cortex-M4 architecture
- **AI Collaboration**: Documentation of human-AI development workflow via GHCP_COMMENTS.md
- **Best Practices**: Memory management, real-time system design, peripheral utilization
- **Reference Material**: Official TI documentation links and technical specifications

## 🔍 Quick Navigation

### By Topic
- **Hardware Platform**: [TM4C1294XL Platform](./TM4C1294XL-Platform.md) → [Hardware Requirements](./Hardware-Requirements.md)
- **Software Features**: [Features](./Features.md) → [README Usage Section](../README.md#usage)
- **Development Journey**: [GHCP_COMMENTS.md](../GHCP_COMMENTS.md) → [Features Diagnostic System](./Features.md#diagnostic-system)

### By Development Phase
1. **Getting Started**: [README.md](../README.md) → [Hardware Requirements](./Hardware-Requirements.md)
2. **Understanding the System**: [Features](./Features.md) → [TM4C1294XL Platform](./TM4C1294XL-Platform.md)
3. **Advanced Development**: [GHCP_COMMENTS.md](../GHCP_COMMENTS.md) → All technical documentation

### By Use Case
- **Quick Setup**: README.md build and usage sections
- **Feature Implementation**: Features.md with code references  
- **Hardware Integration**: Hardware-Requirements.md and TM4C1294XL-Platform.md
- **Learning/Research**: GHCP_COMMENTS.md with AI collaboration insights

## 📊 Documentation Statistics

```
Total Documentation: ~15,000 words
Code References: 50+ examples
Official Links: 15+ TI documentation references
Technical Diagrams: Memory maps, pin assignments, clock trees
Performance Data: Timing specs, power consumption, real-time metrics
```

## 🔄 Documentation Maintenance

This documentation is maintained alongside code development:
- **Code Changes**: Update corresponding documentation sections
- **Feature Additions**: Add entries to Features.md with implementation details
- **Hardware Modifications**: Update Hardware-Requirements.md specifications
- **Major Breakthroughs**: Add entries to GHCP_COMMENTS.md technical blog

---

## 🔖 Project Checkpoints (Jan 2026)

To make it easy to revert or compare behavior across major debugging sessions, we maintain named git tags.

### 1) Recovered snapshot (pre-PWMIN GPIO fallback)

- Tag: `snapshot_integr_v03_20260113a`
- Checkout:
	- `git checkout snapshot_integr_v03_20260113a`

This corresponds to the “last-known-good” restored snapshot before the PWM-input sensing work that added the GPIO edge-timestamp fallback.

### 2) New baseline (“updated as of now”)

- Tag: `baseline_20260117_pwmin_gpio_fallback` (created in the same Jan 17 2026 session)
- Checkout:
	- `git checkout baseline_20260117_pwmin_gpio_fallback`

Key deltas vs the recovered snapshot are documented in the `pwmin.c / pwmin.h` section of:

- [Firmware-Function-Reference.md](./Firmware-Function-Reference.md)

---

**Documentation Version**: 1.1 | **Last Updated (docs)**: January 2026 | **Status**: Active Development